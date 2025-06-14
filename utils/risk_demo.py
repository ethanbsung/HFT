#!/usr/bin/env python3
"""
Live Risk Management Demo
Shows real-time risk monitoring during simulated trading
"""

import time
import random
from datetime import datetime, timezone
from quote_engine import QuoteEngine
from risk_monitor import RiskMonitor

def simulate_market_data():
    """Generate realistic market data stream"""
    base_price = 50000.0
    
    while True:
        # Simulate price movement
        price_change = random.gauss(0, 10)  # $10 std dev
        base_price += price_change
        base_price = max(45000, min(55000, base_price))  # Keep in reasonable range
        
        # Create orderbook
        spread = random.uniform(1, 5)
        bid_price = base_price - spread/2
        ask_price = base_price + spread/2
        
        orderbook = {
            'bids': [[bid_price, random.uniform(0.5, 2.0)]],
            'asks': [[ask_price, random.uniform(0.5, 2.0)]],
            'timestamp': datetime.now(timezone.utc)
        }
        
        # Simulate trades
        trade_price = random.choice([bid_price, ask_price])
        trade_size = random.uniform(0.01, 0.1)
        trade_side = "buy" if trade_price == ask_price else "sell"
        
        yield orderbook, (trade_price, trade_size, trade_side)

def run_risk_demo():
    """Run interactive risk management demo"""
    print("🚀 HFT Risk Management Live Demo")
    print("=" * 60)
    print("This demo simulates live trading with real-time risk monitoring")
    print("Watch for risk alerts and emergency stops!")
    print("=" * 60)
    
    # Initialize systems
    quote_engine = QuoteEngine(max_position_size=0.15)
    risk_monitor = RiskMonitor(quote_engine)
    market_gen = simulate_market_data()
    
    # Demo scenarios
    scenarios = [
        ("Normal Trading", 50, 0.05),
        ("High Volume", 30, 0.08), 
        ("Position Building", 40, 0.12),  # Will approach position limits
        ("Loss Scenario", 25, 0.15),      # Will trigger some risk warnings
        ("Recovery", 35, 0.05)
    ]
    
    iteration = 0
    scenario_index = 0
    
    try:
        for orderbook, trade_data in market_gen:
            iteration += 1
            
            # Switch scenarios every 50 iterations
            if iteration % 50 == 1 and scenario_index < len(scenarios):
                scenario_name, target_orders, target_size = scenarios[scenario_index]
                print(f"\n🎯 SCENARIO: {scenario_name}")
                print("-" * 40)
                scenario_index += 1
            
            # Update quote engine with market data
            quote_engine.update_order_with_orderbook(orderbook)
            
            # Simulate fills
            trade_price, trade_size, trade_side = trade_data
            if random.random() < 0.3:  # 30% chance of trade affecting our orders
                quote_engine.simulate_fill(trade_price, trade_size, trade_side)
            
            # Place orders based on current scenario
            if iteration % 10 == 0:  # Try to place orders every 10 ticks
                mid_price = (orderbook['bids'][0][0] + orderbook['asks'][0][0]) / 2
                
                # Place bid
                bid_price = mid_price - random.uniform(5, 15)
                if random.random() < 0.7:  # 70% chance
                    quote_engine.place_order("buy", bid_price, target_size, orderbook)
                
                # Place ask  
                ask_price = mid_price + random.uniform(5, 15)
                if random.random() < 0.7:  # 70% chance
                    quote_engine.place_order("sell", ask_price, target_size, orderbook)
            
            # Risk monitoring
            if iteration % 5 == 0:  # Check risk every 5 ticks
                risk_monitor.check_and_alert()
            
            # Print status
            if iteration % 15 == 0:  # Print status every 15 ticks
                mid_price = (orderbook['bids'][0][0] + orderbook['asks'][0][0]) / 2
                quote_engine.print_status(mid_price, force=True)
                
                # Print compact risk dashboard
                risk_monitor.print_dashboard(compact=True)
            
            # Full risk dashboard periodically
            if iteration % 100 == 0:
                print("\n" + "="*60)
                print("📊 COMPREHENSIVE RISK DASHBOARD")
                risk_monitor.print_dashboard(compact=False)
                print("="*60)
            
            # Exit conditions
            if iteration >= 300:  # Run for 300 iterations
                break
                
            # Emergency stop check
            if quote_engine.risk_manager.emergency_risk_shutdown():
                print("\n🚨 EMERGENCY STOP TRIGGERED!")
                print("Demo terminated due to risk violations")
                break
            
            # Simulate processing delay
            time.sleep(0.1)
            
    except KeyboardInterrupt:
        print("\n⏹️ Demo stopped by user")
    
    # Final summary
    print("\n" + "="*60)
    print("📋 FINAL SESSION SUMMARY")
    print("="*60)
    
    summary = quote_engine.get_session_performance_summary()
    
    print(f"Session Duration: {summary['session_duration_hours']:.2f} hours")
    print(f"Final PnL: ${summary['final_mtm_pnl']:.2f}")
    print(f"Total Trades: {summary['total_trades']}")
    print(f"Order-to-Trade Ratio: {summary['order_to_trade_ratio']:.1f}")
    print(f"Win Rate: {summary['win_rate_pct']:.1f}%")
    print(f"Sharpe Ratio: {summary['sharpe_ratio']:.3f}")
    print(f"Max Drawdown: {summary['max_drawdown_pct']:.2f}%")
    print(f"Performance Grade: {summary['performance_grade']}")
    
    print("\n🛡️ Risk Management Summary:")
    risk_metrics = summary.get('risk_metrics', {})
    print(f"Active Risk Breaches: {len(risk_metrics.get('active_risk_breaches', []))}")
    print(f"Position Utilization: {risk_metrics.get('risk_utilization', {}).get('position', 'N/A')}")
    print(f"Drawdown Utilization: {risk_metrics.get('risk_utilization', {}).get('drawdown', 'N/A')}")
    
    print("\n⚡ Latency Summary:")
    latency_metrics = summary.get('latency_metrics', {})
    for lat_type, metrics in latency_metrics.items():
        if isinstance(metrics, dict) and 'p95_ms' in metrics:
            print(f"  {lat_type.replace('_', ' ').title()}: {metrics['p95_ms']:.1f}ms (P95)")
    
    print(f"\nDemo completed successfully! ✅")

if __name__ == "__main__":
    run_risk_demo() 