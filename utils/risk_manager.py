#!/usr/bin/env python3
"""
Real-time Risk Management for HFT Systems

This module provides comprehensive risk monitoring and position control
for high-frequency trading operations.
"""

from datetime import datetime, timezone, timedelta
from dataclasses import dataclass
from typing import Dict, Tuple, Optional
import math

@dataclass
class RiskLimits:
    """Risk limit configuration"""
    max_position: float = 0.5          # Maximum position size
    max_daily_loss: float = 1000.0     # Maximum daily loss
    max_drawdown: float = 0.05         # Maximum drawdown (5%)
    position_concentration: float = 0.3 # Max % of typical volume
    var_limit: float = 500.0           # Value at Risk limit
    max_orders_per_second: int = 100   # Order rate limit
    max_latency_ms: float = 50.0       # Maximum acceptable latency

class RiskManager:
    """Real-time risk management for HFT trading"""
    
    def __init__(self, limits: RiskLimits):
        self.limits = limits
        self.session_start = datetime.now(timezone.utc)
        self.daily_pnl = 0.0
        self.peak_equity = 1000.0  # Start with initial capital, not 100k
        self.max_drawdown_observed = 0.0
        
        # Order rate limiting
        self.order_timestamps = []
        self.order_window_seconds = 1
        
        # Position tracking
        self.position_history = []
        self.pnl_history = []
        
        # Risk state
        self.risk_breaches = set()
        self.last_risk_check = None
        
    def check_pre_trade_risk(self, side: str, size: float, price: float, 
                            current_position: float, current_equity: float,
                            latency_ms: float) -> Tuple[bool, Dict[str, bool]]:
        """
        Comprehensive pre-trade risk check
        Returns: (can_trade, risk_checks_detail)
        """
        now = datetime.now(timezone.utc)
        self.last_risk_check = now
        
        # Calculate hypothetical new position
        position_delta = size if side == "buy" else -size
        new_position = current_position + position_delta
        
        # Update daily PnL (simplified - should track more precisely)
        current_pnl = current_equity - self.peak_equity
        
        # Be more lenient during initial startup - allow small negative PnL fluctuations
        session_duration_minutes = (now - self.session_start).total_seconds() / 60
        if session_duration_minutes < 5.0:  # First 5 minutes are grace period
            # During startup, only trigger if loss is significant (> 1% of capital)
            daily_pnl_ok = current_pnl >= -(self.peak_equity * 0.01)
        else:
            daily_pnl_ok = current_pnl >= -self.limits.max_daily_loss
        
        # Risk checks
        checks = {
            'position_limit': abs(new_position) <= self.limits.max_position,
            'daily_pnl_limit': daily_pnl_ok,
            'drawdown_limit': self._check_drawdown_limit(current_equity),
            'concentration_risk': self._check_concentration_risk(size, price),
            'var_limit': self._check_var_limit(new_position, price),
            'order_rate_limit': self._check_order_rate_limit(now),
            'latency_limit': latency_ms <= self.limits.max_latency_ms,
            'no_critical_breaches': len(self.risk_breaches) == 0
        }
        
        # Update risk breach tracking
        self._update_risk_breaches(checks)
        
        # Log risk violations
        violations = [check for check, passed in checks.items() if not passed]
        if violations:
            print(f"🚨 RISK VIOLATION: {', '.join(violations)}")
            
        can_trade = all(checks.values())
        return can_trade, checks
    
    def update_position_and_pnl(self, new_position: float, new_equity: float):
        """Update position and PnL tracking for risk monitoring"""
        now = datetime.now(timezone.utc)
        
        # Update position history
        self.position_history.append((now, new_position))
        
        # Update PnL history
        current_pnl = new_equity - self.peak_equity
        self.pnl_history.append((now, current_pnl))
        self.daily_pnl = current_pnl
        
        # Update peak equity and drawdown
        if new_equity > self.peak_equity:
            self.peak_equity = new_equity
        else:
            current_drawdown = (self.peak_equity - new_equity) / self.peak_equity
            if current_drawdown > self.max_drawdown_observed:
                self.max_drawdown_observed = current_drawdown
                
        # Clean old data (keep last hour)
        cutoff_time = now - timedelta(hours=1)
        self.position_history = [(t, p) for t, p in self.position_history if t > cutoff_time]
        self.pnl_history = [(t, p) for t, p in self.pnl_history if t > cutoff_time]
    
    def record_order_attempt(self):
        """Record order attempt for rate limiting"""
        now = datetime.now(timezone.utc)
        self.order_timestamps.append(now)
        
        # Clean old timestamps
        cutoff_time = now - timedelta(seconds=self.order_window_seconds)
        self.order_timestamps = [t for t in self.order_timestamps if t > cutoff_time]
    
    def _check_drawdown_limit(self, current_equity: float) -> bool:
        """Check if drawdown is within limits"""
        if self.peak_equity <= 0:
            return True
            
        # Be more lenient during startup - small fluctuations in equity are normal
        session_duration_minutes = (datetime.now(timezone.utc) - self.session_start).total_seconds() / 60
        
        current_drawdown = (self.peak_equity - current_equity) / self.peak_equity
        
        if session_duration_minutes < 5.0:  # First 5 minutes grace period
            # Allow up to 2% drawdown during startup (market data quirks, etc.)
            return current_drawdown <= 0.02
        else:
            return current_drawdown <= self.limits.max_drawdown
    
    def _check_concentration_risk(self, size: float, price: float) -> bool:
        """Check if order size is reasonable relative to typical market volume"""
        # Symbol-aware volume estimation based on market cap and price range
        order_value = size * price
        
        # Estimate typical minute volume based on price range (proxy for market size)
        if price >= 50000:  # BTC range
            typical_minute_volume_tokens = 10.0
        elif price >= 1000:  # ETH range  
            typical_minute_volume_tokens = 50.0
        elif price >= 100:  # Mid-cap coins
            typical_minute_volume_tokens = 100.0
        elif price >= 10:   # Small-cap coins
            typical_minute_volume_tokens = 500.0
        elif price >= 1:    # Micro-cap coins  
            typical_minute_volume_tokens = 1000.0
        else:               # Very low-price tokens (like DEXT at $0.33)
            typical_minute_volume_tokens = 2000.0
        
        # Calculate max order value as percentage of typical volume
        max_order_value = typical_minute_volume_tokens * self.limits.position_concentration * price
        
        # Additional safeguard: for very small orders, always allow
        min_reasonable_order_value = 0.50  # $0.50 minimum
        if order_value <= min_reasonable_order_value:
            return True
            
        return order_value <= max_order_value
    
    def _check_var_limit(self, position: float, current_price: float) -> bool:
        """Simplified Value at Risk check"""
        # Assume 1% daily volatility for BTC
        daily_volatility = 0.01
        confidence_level = 0.99  # 99% VaR
        
        # Simplified VaR calculation
        position_value = abs(position * current_price)
        var_estimate = position_value * daily_volatility * 2.33  # 99% quantile
        
        return var_estimate <= self.limits.var_limit
    
    def _check_order_rate_limit(self, now: datetime) -> bool:
        """Check if order rate is within limits"""
        recent_orders = len(self.order_timestamps)
        return recent_orders <= self.limits.max_orders_per_second
    
    def _update_risk_breaches(self, checks: Dict[str, bool]):
        """Update critical risk breach tracking"""
        critical_checks = ['position_limit', 'daily_pnl_limit', 'drawdown_limit']
        
        for check in critical_checks:
            if not checks.get(check, True):
                self.risk_breaches.add(check)
            elif check in self.risk_breaches:
                # Risk resolved
                self.risk_breaches.discard(check)
    
    def get_risk_summary(self) -> Dict:
        """Get comprehensive risk summary"""
        now = datetime.now(timezone.utc)
        session_duration = (now - self.session_start).total_seconds() / 3600
        
        # Calculate position volatility
        if len(self.position_history) > 1:
            positions = [p for _, p in self.position_history]
            position_std = self._calculate_std(positions)
        else:
            position_std = 0.0
            
        # Calculate recent order rate
        recent_order_rate = len(self.order_timestamps) / max(1, self.order_window_seconds)
        
        return {
            'session_duration_hours': round(session_duration, 2),
            'daily_pnl': round(self.daily_pnl, 2),
            'max_drawdown_pct': round(self.max_drawdown_observed * 100, 3),
            'position_volatility': round(position_std, 4),
            'recent_order_rate': round(recent_order_rate, 1),
            'active_risk_breaches': list(self.risk_breaches),
            'risk_utilization': {
                'position': f"{(abs(self.position_history[-1][1]) / self.limits.max_position * 100):.1f}%" if self.position_history else "0%",
                'daily_loss': f"{(abs(min(0, self.daily_pnl)) / self.limits.max_daily_loss * 100):.1f}%",
                'drawdown': f"{(self.max_drawdown_observed / self.limits.max_drawdown * 100):.1f}%"
            }
        }
    
    def _calculate_std(self, values):
        """Calculate standard deviation"""
        if len(values) < 2:
            return 0.0
        mean_val = sum(values) / len(values)
        variance = sum((x - mean_val) ** 2 for x in values) / (len(values) - 1)
        return math.sqrt(variance)
    
    def emergency_risk_shutdown(self) -> bool:
        """Check if emergency shutdown is required"""
        emergency_conditions = [
            self.daily_pnl < -self.limits.max_daily_loss * 0.8,  # 80% of daily limit
            self.max_drawdown_observed > self.limits.max_drawdown * 0.9,  # 90% of DD limit
            len(self.risk_breaches) >= 2  # Multiple critical breaches
        ]
        
        return any(emergency_conditions)

# Example usage and integration
class InventoryManager:
    """Advanced inventory management with risk-aware skewing"""
    
    def __init__(self, target_inventory: float = 0.0, max_inventory: float = 0.5):
        self.target_inventory = target_inventory
        self.max_inventory = max_inventory
        self.inventory_half_life = 300  # seconds
        self.last_inventory_time = datetime.now(timezone.utc)
        
    def get_inventory_skew(self, current_inventory: float, current_mid_price: float,
                          market_volatility: float) -> Tuple[float, float]:
        """
        Calculate inventory-based price skew for bid/ask placement
        Returns: (bid_skew_ticks, ask_skew_ticks)
        """
        now = datetime.now(timezone.utc)
        time_since_last = (now - self.last_inventory_time).total_seconds()
        
        # Calculate inventory deviation from target
        inventory_deviation = current_inventory - self.target_inventory
        
        # Risk penalty increases with inventory size and age
        inventory_risk = abs(inventory_deviation) / self.max_inventory
        time_penalty = min(1.0, time_since_last / self.inventory_half_life)
        
        # Base skew proportional to inventory
        base_skew_ticks = inventory_deviation * 10  # 10 ticks per unit inventory
        
        # Volatility adjustment
        vol_multiplier = 1.0 + market_volatility * 2.0
        
        # Risk-adjusted skew
        risk_adjusted_skew = base_skew_ticks * (1 + inventory_risk * time_penalty) * vol_multiplier
        
        # Apply skew to bid/ask
        bid_skew = -risk_adjusted_skew / 2  # Negative skew = tighter bid
        ask_skew = risk_adjusted_skew / 2   # Positive skew = wider ask
        
        return bid_skew, ask_skew
    
    def update_inventory_time(self):
        """Update last inventory tracking time"""
        self.last_inventory_time = datetime.now(timezone.utc)

def example_risk_integration():
    """Example of how to integrate risk management with trading"""
    
    # Initialize risk management
    limits = RiskLimits(
        max_position=0.5,
        max_daily_loss=1000.0,
        max_drawdown=0.05
    )
    risk_manager = RiskManager(limits)
    inventory_manager = InventoryManager()
    
    # Example pre-trade check
    can_trade, risk_details = risk_manager.check_pre_trade_risk(
        side="buy",
        size=0.1,
        price=50000.0,
        current_position=0.2,
        current_equity=99500.0,
        latency_ms=25.0
    )
    
    print(f"Can trade: {can_trade}")
    print(f"Risk details: {risk_details}")
    
    # Example inventory skew calculation
    bid_skew, ask_skew = inventory_manager.get_inventory_skew(
        current_inventory=0.2,
        current_mid_price=50000.0,
        market_volatility=0.02
    )
    
    print(f"Inventory skew - Bid: {bid_skew:.1f} ticks, Ask: {ask_skew:.1f} ticks")

if __name__ == "__main__":
    example_risk_integration() 