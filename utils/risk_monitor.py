#!/usr/bin/env python3
"""
Risk Monitoring Dashboard for HFT System
Provides real-time risk monitoring and alerting
"""

import time
import json
from datetime import datetime, timezone
from risk_manager import RiskManager, RiskLimits, InventoryManager

class RiskMonitor:
    """Real-time risk monitoring dashboard"""
    
    def __init__(self, quote_engine=None):
        self.quote_engine = quote_engine
        self.last_alert_time = None
        self.alert_cooldown = 30  # seconds between alerts
        
    def get_comprehensive_risk_report(self):
        """Generate comprehensive risk report"""
        if not self.quote_engine:
            return {"error": "No quote engine connected"}
        
        # Get current state
        current_position = self.quote_engine.position
        current_pnl = self.quote_engine.spread_capture_pnl
        current_cash = self.quote_engine.cash
        current_mtm = self.quote_engine.mark_to_market(50000)  # Use mid-price estimate
        
        # Risk manager status
        risk_summary = self.quote_engine.risk_manager.get_risk_summary()
        
        # Trading metrics
        ot_ratio = self.quote_engine.get_order_to_trade_ratio()
        sharpe = self.quote_engine.calculate_sharpe_ratio()
        win_rate = self.quote_engine.get_win_rate()
        
        # Latency metrics
        latency_summary = self.quote_engine.latency_tracker.get_latency_summary()
        
        # Risk grade calculation
        risk_grade = self._calculate_risk_grade(risk_summary, ot_ratio, latency_summary)
        
        report = {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "position_metrics": {
                "current_position": current_position,
                "max_position_limit": self.quote_engine.max_position_size,
                "position_utilization_pct": (abs(current_position) / self.quote_engine.max_position_size) * 100,
                "inventory_deviation": current_position - self.quote_engine.inventory_manager.target_inventory
            },
            "pnl_metrics": {
                "spread_capture_pnl": current_pnl,
                "cash_position": current_cash,
                "mark_to_market": current_mtm,
                "total_fees_paid": self.quote_engine.total_fees_paid,
                "max_drawdown_pct": self.quote_engine.max_drawdown_observed * 100
            },
            "trading_metrics": {
                "orders_sent": self.quote_engine.orders_sent,
                "trades_filled": self.quote_engine.trades_filled,
                "order_to_trade_ratio": ot_ratio,
                "sharpe_ratio": sharpe,
                "win_rate_pct": win_rate
            },
            "risk_summary": risk_summary,
            "latency_metrics": latency_summary,
            "risk_grade": risk_grade,
            "emergency_stop_required": self.quote_engine.risk_manager.emergency_risk_shutdown(),
            "active_orders": {
                "bid_order": {
                    "price": self.quote_engine.open_bid_order.price if self.quote_engine.open_bid_order else None,
                    "size": self.quote_engine.open_bid_order.remaining_qty if self.quote_engine.open_bid_order else None,
                    "queue": self.quote_engine.open_bid_order.current_queue if self.quote_engine.open_bid_order else None
                },
                "ask_order": {
                    "price": self.quote_engine.open_ask_order.price if self.quote_engine.open_ask_order else None,
                    "size": self.quote_engine.open_ask_order.remaining_qty if self.quote_engine.open_ask_order else None,
                    "queue": self.quote_engine.open_ask_order.current_queue if self.quote_engine.open_ask_order else None
                }
            }
        }
        
        return report
    
    def _calculate_risk_grade(self, risk_summary, ot_ratio, latency_summary):
        """Calculate overall risk grade A-F"""
        score = 100
        
        # Deduct points for risk violations
        active_breaches = len(risk_summary.get('active_risk_breaches', []))
        score -= active_breaches * 15
        
        # Deduct for emergency conditions
        if risk_summary.get('emergency_stops', 0) > 0:
            score -= 30
        
        # Deduct for excessive O:T ratio
        if ot_ratio > 25:
            score -= 20
        elif ot_ratio > 15:
            score -= 10
        
        # Deduct for latency issues
        if latency_summary and latency_summary.get('critical_spikes', 0) > 0:
            score -= 15
        
        # Convert to letter grade
        if score >= 90:
            return "A"
        elif score >= 80:
            return "B"
        elif score >= 70:
            return "C"
        elif score >= 60:
            return "D"
        else:
            return "F"
    
    def check_and_alert(self):
        """Check for risk violations and alert if necessary"""
        if not self.quote_engine:
            return
        
        now = datetime.now(timezone.utc)
        
        # Check cooldown
        if self.last_alert_time and (now - self.last_alert_time).total_seconds() < self.alert_cooldown:
            return
        
        risk_summary = self.quote_engine.risk_manager.get_risk_summary()
        emergency_stop = self.quote_engine.risk_manager.emergency_risk_shutdown()
        
        # Check for critical violations
        critical_violations = risk_summary.get('active_risk_breaches', [])
        
        if emergency_stop or critical_violations:
            self._send_alert(critical_violations, emergency_stop)
            self.last_alert_time = now
    
    def _send_alert(self, violations, emergency_stop):
        """Send risk alert (in production, this would integrate with alerting system)"""
        alert_level = "🚨 CRITICAL" if emergency_stop else "⚠️ WARNING"
        
        print(f"\n{alert_level} RISK ALERT - {datetime.now(timezone.utc).isoformat()}")
        print("=" * 60)
        
        if emergency_stop:
            print("EMERGENCY STOP CONDITION TRIGGERED!")
            print("All trading should be halted immediately.")
        
        if violations:
            print(f"Risk violations detected: {', '.join(violations)}")
        
        # Get comprehensive report
        report = self.get_comprehensive_risk_report()
        
        print(f"Position: {report['position_metrics']['current_position']:.4f}")
        print(f"PnL: {report['pnl_metrics']['spread_capture_pnl']:.2f}")
        print(f"O:T Ratio: {report['trading_metrics']['order_to_trade_ratio']:.1f}")
        print(f"Risk Grade: {report['risk_grade']}")
        print("=" * 60)
    
    def print_dashboard(self, compact=False):
        """Print risk dashboard to console"""
        report = self.get_comprehensive_risk_report()
        
        if compact:
            self._print_compact_dashboard(report)
        else:
            self._print_full_dashboard(report)
    
    def _print_compact_dashboard(self, report):
        """Print compact one-line risk summary"""
        pos = report['position_metrics']['current_position']
        pnl = report['pnl_metrics']['spread_capture_pnl']
        ot_ratio = report['trading_metrics']['order_to_trade_ratio']
        risk_grade = report['risk_grade']
        
        violations = len(report['risk_summary'].get('active_risk_breaches', []))
        emergency = "🚨" if report['emergency_stop_required'] else "✅"
        
        print(f"RISK: {emergency} Grade:{risk_grade} | Pos:{pos:.3f} PnL:{pnl:.2f} O:T:{ot_ratio:.1f} Violations:{violations}")
    
    def _print_full_dashboard(self, report):
        """Print full risk dashboard"""
        print("\n" + "=" * 80)
        print(f"HFT RISK MANAGEMENT DASHBOARD - {report['timestamp']}")
        print("=" * 80)
        
        # Position metrics
        print("\n📊 POSITION METRICS:")
        pos_metrics = report['position_metrics']
        print(f"  Current Position: {pos_metrics['current_position']:.6f}")
        print(f"  Position Limit: ±{pos_metrics['max_position_limit']:.6f}")
        print(f"  Utilization: {pos_metrics['position_utilization_pct']:.1f}%")
        print(f"  Inventory Deviation: {pos_metrics['inventory_deviation']:.6f}")
        
        # PnL metrics
        print("\n💰 PNL METRICS:")
        pnl_metrics = report['pnl_metrics']
        print(f"  Spread Capture PnL: {pnl_metrics['spread_capture_pnl']:.2f}")
        print(f"  Cash Position: {pnl_metrics['cash_position']:.2f}")
        print(f"  Mark-to-Market: {pnl_metrics['mark_to_market']:.2f}")
        print(f"  Total Fees: {pnl_metrics['total_fees_paid']:.4f}")
        print(f"  Max Drawdown: {pnl_metrics['max_drawdown_pct']:.2f}%")
        
        # Trading metrics
        print("\n📈 TRADING METRICS:")
        trading_metrics = report['trading_metrics']
        print(f"  Orders Sent: {trading_metrics['orders_sent']}")
        print(f"  Trades Filled: {trading_metrics['trades_filled']}")
        print(f"  Order-to-Trade Ratio: {trading_metrics['order_to_trade_ratio']:.1f}")
        print(f"  Sharpe Ratio: {trading_metrics['sharpe_ratio']:.3f}")
        print(f"  Win Rate: {trading_metrics['win_rate_pct']:.1f}%")
        
        # Risk status
        print("\n🛡️ RISK STATUS:")
        risk_summary = report['risk_summary']
        active_breaches = risk_summary.get('active_risk_breaches', [])
        
        if active_breaches:
            print("\n⚠️ ACTIVE RISK BREACHES:")
            for breach in active_breaches:
                print(f"  ❌ {breach.replace('_', ' ').title()}")
        else:
            print("  ✅ All risk checks passing")
        
        # Risk utilization
        risk_util = risk_summary.get('risk_utilization', {})
        if risk_util:
            print("\n📊 RISK UTILIZATION:")
            for metric, value in risk_util.items():
                print(f"  {metric.replace('_', ' ').title()}: {value}")
        
        # Latency metrics
        if report['latency_metrics']:
            print("\n⚡ LATENCY METRICS:")
            lat_metrics = report['latency_metrics']
            for lat_type, metrics in lat_metrics.items():
                if lat_type not in ['recent_spikes', 'critical_spikes'] and isinstance(metrics, dict):
                    print(f"  {lat_type.replace('_', ' ').title()}: {metrics['p95_ms']:.1f}ms (P95)")
        
        # Overall assessment
        print(f"\n🎯 RISK GRADE: {report['risk_grade']}")
        if report['emergency_stop_required']:
            print("🚨 EMERGENCY STOP REQUIRED!")
        
        print("=" * 80)

def main():
    """Demonstrate risk monitoring without live trading"""
    print("HFT Risk Management System - Demonstration Mode")
    print("=" * 60)
    
    # Create risk limits
    risk_limits = RiskLimits()
    risk_manager = RiskManager(risk_limits)
    inventory_manager = InventoryManager()
    
    # Simulate some risk scenarios
    print("\n1. Normal Trading Conditions:")
    can_trade, risk_details = risk_manager.check_pre_trade_risk(
        side="buy", size=0.05, price=50000, 
        current_position=0.01, current_equity=100000, latency_ms=5.0
    )
    print(f"Can trade: {can_trade}")
    print(f"Risk details: {risk_details}")
    
    print("\n2. High Position Scenario:")
    can_trade_large, _ = risk_manager.check_pre_trade_risk(
        side="buy", size=0.05, price=50000,
        current_position=0.08, current_equity=100000, latency_ms=5.0
    )
    can_trade_small, _ = risk_manager.check_pre_trade_risk(
        side="buy", size=0.01, price=50000,
        current_position=0.08, current_equity=100000, latency_ms=5.0
    )
    print(f"Can trade large: {can_trade_large}")
    print(f"Can trade small: {can_trade_small}")
    
    print("\n3. Loss Scenario:")
    can_trade_loss, _ = risk_manager.check_pre_trade_risk(
        side="buy", size=0.05, price=50000,
        current_position=0.01, current_equity=98500, latency_ms=5.0  # $1500 loss
    )
    emergency_stop = risk_manager.emergency_risk_shutdown()
    print(f"Can trade after loss: {can_trade_loss}")
    print(f"Emergency stop needed: {emergency_stop}")
    
    print("\n4. Inventory Skewing:")
    bid_skew, ask_skew = inventory_manager.get_inventory_skew(
        current_inventory=0.03, current_mid_price=50000, market_volatility=0.02
    )
    print(f"Inventory skew - Bid: {bid_skew:.1f} ticks, Ask: {ask_skew:.1f} ticks")
    
    print("\n5. Risk Summary:")
    summary = risk_manager.get_risk_summary()
    print(f"Risk summary: {summary}")

if __name__ == "__main__":
    main()