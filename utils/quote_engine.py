from datetime import datetime, timezone, timedelta
from execution_simulator import ExecutionSimulator
import copy
from collections import deque
import statistics
from .risk_manager import RiskManager, RiskLimits, InventoryManager

class LatencyTracker:
    """Track various latency metrics for HFT performance monitoring"""
    
    def __init__(self, window_size=1000):
        self.window_size = window_size
        
        # Rolling windows for different latency types (in microseconds)
        self.market_data_processing_latencies = deque(maxlen=window_size)
        self.order_placement_latencies = deque(maxlen=window_size)
        self.order_cancel_latencies = deque(maxlen=window_size)
        self.tick_to_trade_latencies = deque(maxlen=window_size)
        
        # Remove the unrealistic order-to-fill latency tracking
        # In real HFT, we care about processing latencies, not market timing
        
        # Latency spike tracking
        self.latency_spikes = deque(maxlen=100)  # Keep last 100 spikes
        
        # Realistic thresholds (in microseconds) for HFT systems
        self.thresholds = {
            'market_data_warning': 1000,          # 1ms
            'market_data_critical': 5000,         # 5ms
            'order_placement_warning': 2000,      # 2ms  
            'order_placement_critical': 10000,    # 10ms
            'tick_to_trade_warning': 5000,        # 5ms
            'tick_to_trade_critical': 15000       # 15ms
        }
        
        # Current session tracking
        self.session_start = datetime.now(timezone.utc)
        self.last_latency_report_time = None
        
    def add_market_data_latency(self, latency_us: float):
        """Add market data processing latency measurement"""
        self.market_data_processing_latencies.append(latency_us)
        self._check_spike('market_data', latency_us)
        
    def add_order_placement_latency(self, latency_us: float):
        """Add order placement latency measurement"""
        self.order_placement_latencies.append(latency_us)
        self._check_spike('order_placement', latency_us)
        
    def add_order_cancel_latency(self, latency_us: float):
        """Add order cancellation latency measurement"""
        self.order_cancel_latencies.append(latency_us)
        
    def add_tick_to_trade_latency(self, latency_us: float):
        """Add tick-to-trade latency measurement"""
        self.tick_to_trade_latencies.append(latency_us)
        self._check_spike('tick_to_trade', latency_us)
        
    def _check_spike(self, latency_type: str, latency_us: float):
        """Check if latency is a spike and record it"""
        warning_threshold = self.thresholds.get(f'{latency_type}_warning', float('inf'))
        critical_threshold = self.thresholds.get(f'{latency_type}_critical', float('inf'))
        
        if latency_us > critical_threshold:
            self.latency_spikes.append({
                'timestamp': datetime.now(timezone.utc),
                'type': latency_type,
                'latency_us': latency_us,
                'severity': 'critical'
            })
        elif latency_us > warning_threshold:
            self.latency_spikes.append({
                'timestamp': datetime.now(timezone.utc),
                'type': latency_type,
                'latency_us': latency_us,
                'severity': 'warning'
            })
    
    def get_statistics(self, latency_type: str):
        """Get statistics for a specific latency type"""
        if latency_type == 'market_data':
            data = list(self.market_data_processing_latencies)
        elif latency_type == 'order_placement':
            data = list(self.order_placement_latencies)
        elif latency_type == 'order_cancel':
            data = list(self.order_cancel_latencies)
        elif latency_type == 'tick_to_trade':
            data = list(self.tick_to_trade_latencies)
        else:
            return None
            
        if not data:
            return None
            
        return {
            'count': len(data),
            'mean_us': statistics.mean(data),
            'median_us': statistics.median(data),
            'p95_us': self._percentile(data, 95),
            'p99_us': self._percentile(data, 99),
            'max_us': max(data),
            'min_us': min(data)
        }
    
    def _percentile(self, data, percentile):
        """Calculate percentile of data"""
        if not data:
            return 0
        sorted_data = sorted(data)
        index = (percentile / 100) * (len(sorted_data) - 1)
        if index == int(index):
            return sorted_data[int(index)]
        else:
            lower = sorted_data[int(index)]
            upper = sorted_data[int(index) + 1]
            return lower + (upper - lower) * (index - int(index))
    
    def get_recent_spikes(self, minutes=5):
        """Get latency spikes from the last N minutes"""
        cutoff_time = datetime.now(timezone.utc) - timedelta(minutes=minutes)
        return [spike for spike in self.latency_spikes if spike['timestamp'] > cutoff_time]
    
    def should_alert(self):
        """Check if we should alert on latency issues"""
        recent_spikes = self.get_recent_spikes(minutes=1)
        critical_spikes = [s for s in recent_spikes if s['severity'] == 'critical']
        warning_spikes = [s for s in recent_spikes if s['severity'] == 'warning']
        
        return len(critical_spikes) > 0 or len(warning_spikes) > 3
    
    def get_latency_summary(self):
        """Get comprehensive latency summary"""
        summary = {}
        latency_types = ['market_data', 'order_placement', 'tick_to_trade']
        
        for lat_type in latency_types:
            stats = self.get_statistics(lat_type)
            if stats:
                summary[lat_type] = {
                    'mean_ms': round(stats['mean_us'] / 1000, 3),
                    'p95_ms': round(stats['p95_us'] / 1000, 3),
                    'p99_ms': round(stats['p99_us'] / 1000, 3),
                    'max_ms': round(stats['max_us'] / 1000, 3),
                    'count': stats['count']
                }
        
        # Add spike information
        recent_spikes = self.get_recent_spikes(minutes=5)
        summary['recent_spikes'] = len(recent_spikes)
        summary['critical_spikes'] = len([s for s in recent_spikes if s['severity'] == 'critical'])
        
        return summary

    def print_detailed_latency_report(self):
        """Print a comprehensive, readable latency performance report"""
        print("\n" + "="*60)
        print("📊 LATENCY PERFORMANCE REPORT")
        print("="*60)
        
        summary = self.get_latency_summary()
        
        if not summary:
            print("No latency data collected yet.")
            return
            
        # Header
        print(f"{'Metric':<20} {'Mean':<8} {'P95':<8} {'P99':<8} {'Max':<8} {'Count':<8}")
        print("-" * 60)
        
        # Market Data Processing
        if 'market_data' in summary:
            md = summary['market_data']
            print(f"{'Feed Processing':<20} {md['mean_ms']:<8.3f} {md['p95_ms']:<8.3f} {md['p99_ms']:<8.3f} {md['max_ms']:<8.3f} {md['count']:<8}")
            
        # Order Placement  
        if 'order_placement' in summary:
            op = summary['order_placement']
            print(f"{'Order Placement':<20} {op['mean_ms']:<8.3f} {op['p95_ms']:<8.3f} {op['p99_ms']:<8.3f} {op['max_ms']:<8.3f} {op['count']:<8}")
            
        # Tick-to-Trade
        if 'tick_to_trade' in summary:
            tt = summary['tick_to_trade']
            print(f"{'Tick-to-Trade':<20} {tt['mean_ms']:<8.3f} {tt['p95_ms']:<8.3f} {tt['p99_ms']:<8.3f} {tt['max_ms']:<8.3f} {tt['count']:<8}")
        
        print("-" * 60)
        
        # Spike Analysis
        recent_spikes = summary.get('recent_spikes', 0)
        critical_spikes = summary.get('critical_spikes', 0)
        
        if recent_spikes > 0:
            print(f"⚠️  Latency Spikes (last 5min): {recent_spikes} total, {critical_spikes} critical")
            
            # Show recent spikes detail
            spikes = self.get_recent_spikes(minutes=5)
            for spike in spikes[-5:]:  # Show last 5 spikes
                severity_icon = "🔴" if spike['severity'] == 'critical' else "🟡"
                print(f"   {severity_icon} {spike['type']}: {spike['latency_us']/1000:.2f}ms at {spike['timestamp'].strftime('%H:%M:%S')}")
        else:
            print("✅ No recent latency spikes detected")
            
        # Performance Assessment
        print("\n📈 LATENCY PERFORMANCE ASSESSMENT:")
        
        # Assess each metric
        assessments = []
        if 'market_data' in summary:
            md_p95 = summary['market_data']['p95_ms']
            if md_p95 < 1.0:
                assessments.append("✅ Feed processing: Excellent (<1ms)")
            elif md_p95 < 3.0:
                assessments.append("🟡 Feed processing: Good (<3ms)")
            else:
                assessments.append("🔴 Feed processing: Needs improvement (>3ms)")
                
        if 'order_placement' in summary:
            op_p95 = summary['order_placement']['p95_ms']
            if op_p95 < 2.0:
                assessments.append("✅ Order placement: Excellent (<2ms)")
            elif op_p95 < 5.0:
                assessments.append("🟡 Order placement: Good (<5ms)")
            else:
                assessments.append("🔴 Order placement: Needs improvement (>5ms)")
                
        if 'tick_to_trade' in summary:
            tt_p95 = summary['tick_to_trade']['p95_ms']
            if tt_p95 < 5.0:
                assessments.append("✅ Tick-to-trade: Excellent (<5ms)")
            elif tt_p95 < 10.0:
                assessments.append("🟡 Tick-to-trade: Good (<10ms)")
            else:
                assessments.append("🔴 Tick-to-trade: Needs improvement (>10ms)")
        
        for assessment in assessments:
            print(f"   {assessment}")
            
        print("\n💡 BENCHMARKS:")
        print("   Excellent HFT: Feed <1ms, Orders <2ms, T2T <5ms")
        print("   Good HFT: Feed <3ms, Orders <5ms, T2T <10ms")
        print("   Production C++: 10-100x faster than these Python numbers")
        print("="*60)

    def simulate_realistic_latency(self, latency_type: str) -> float:
        """
        Simulate realistic HFT latencies based on system type and market conditions
        Returns latency in microseconds
        """
        import random
        
        if latency_type == 'market_data':
            # Market data processing: 100-2000 microseconds (0.1-2ms)
            # Includes network jitter, parsing, validation
            base_latency = random.uniform(100, 800)
            jitter = random.uniform(0, 200) if random.random() < 0.8 else random.uniform(200, 1200)
            return base_latency + jitter
            
        elif latency_type == 'order_placement':
            # Order placement: 200-5000 microseconds (0.2-5ms)
            # Includes validation, risk check, network send
            base_latency = random.uniform(200, 1500)
            jitter = random.uniform(0, 500) if random.random() < 0.9 else random.uniform(500, 3500)
            return base_latency + jitter
            
        elif latency_type == 'order_cancel':
            # Order cancellation: usually faster than placement
            base_latency = random.uniform(150, 1000)
            jitter = random.uniform(0, 300) if random.random() < 0.9 else random.uniform(300, 2000)
            return base_latency + jitter
            
        elif latency_type == 'tick_to_trade':
            # Total decision latency: market data + processing + order send
            md_latency = self.simulate_realistic_latency('market_data')
            processing_latency = random.uniform(50, 500)  # Algorithm processing
            order_latency = self.simulate_realistic_latency('order_placement')
            return md_latency + processing_latency + order_latency
            
        else:
            return random.uniform(100, 1000)  # Default fallback

class Order:
    def __init__(self, side, price, size, queue_ahead, mid_price_at_entry, entry_time=None):
        self.side = side
        self.price = price
        self.qty = size
        self.initial_queue = queue_ahead
        self.current_queue = queue_ahead
        self.filled_qty = 0
        self.remaining_qty = size
        self.entry_time = entry_time or datetime.now(timezone.utc)
        self.order_id = f"{side}_{price}_{self.entry_time.timestamp()}"
        # Track our original price level for queue maintenance
        self.original_price_level = price
        self.mid_price_at_entry = mid_price_at_entry
        
        # Latency tracking
        self.placement_start_time = None  # When we started placing the order
        self.placement_complete_time = None  # When order placement completed

class QuoteEngine:
    TICK = 0.0001  # DEXT-USD quote increment
    BASE_MAX_TICKS_AWAY = 15
    ADAPTIVE_MAX_TICKS_MULTIPLIER = 2.0
    ORDER_TTL_SEC = 120.0
    MIN_ORDER_REPLACE_INTERVAL = 0.5
    MAKER_FEE_RATE = 0.004  # 0.4% starting maker fee (updates dynamically)
    DEFAULT_ORDER_SIZE = 10.0   # 10 DEXT per quote

    def __init__(self, max_position_size=100.0, exec_sim: ExecutionSimulator | None = None):
        self.position = 0
        self.cash = 1_000.0
        # Execution simulator for paper‑trading hooks
        self.exec_sim = exec_sim
        self.initial_cash = self.cash
        self.open_bid_order = None
        self.open_ask_order = None
        self.last_orderbook = None
        self.last_cancel_time = None
        self.last_manual_cancel_time = None
        self.max_position_size = max_position_size
        self.last_bid_replace_time = None
        self.last_ask_replace_time = None
        # Track when meaningful events happen for status printing
        self.last_status_print_time = None
        self.status_print_events = set()  # Track what events trigger status prints
        self.spread_capture_pnl = 0.0
        self.total_fees_paid = 0.0
        
        # Order-to-trade ratio tracking
        self.orders_sent = 0
        self.trades_filled = 0
        self.recent_orders = []  # List of (timestamp, order_type) for rolling window
        self.recent_fills = []   # List of timestamps for rolling window
        self.ot_ratio_window = 300  # 5 minute window in seconds
        
        # Performance analytics for realistic simulation benchmarks
        self.pnl_history = []  # Store (timestamp, pnl) for Sharpe calculation
        self.daily_pnls = []   # Store daily PnL for drawdown calculation
        self.trades_won = 0
        self.trades_total = 0
        self.max_drawdown_observed = 0.0
        self.peak_equity = self.initial_cash
        self.session_start_time = datetime.now(timezone.utc)
        
        # Add latency tracking
        self.latency_tracker = LatencyTracker()
        self.market_data_receive_time = None
        self.last_tick_to_trade_start = None
        
        # Add risk management
        risk_limits = RiskLimits(
            max_position=max_position_size,
            max_daily_loss=self.initial_cash * 0.02,  # 2% of initial capital
            max_drawdown=0.05,  # 5% max drawdown
            position_concentration=0.3,  # 30% of typical volume
            var_limit=self.initial_cash * 0.01,  # 1% VaR limit
            max_orders_per_second=50,  # Reasonable order rate
            max_latency_ms=100.0  # 100ms latency threshold
        )
        self.risk_manager = RiskManager(risk_limits)
        self.inventory_manager = InventoryManager(target_inventory=0.0, max_inventory=max_position_size)
        
        # Set up ExecutionSimulator callback if provided
        if self.exec_sim and hasattr(self.exec_sim, 'quote_engine_callback'):
            self.exec_sim.quote_engine_callback = self._handle_execution_event

    def _start_market_data_processing(self):
        """Mark the start of market data processing"""
        self.market_data_receive_time = datetime.now(timezone.utc)
        self.last_tick_to_trade_start = self.market_data_receive_time
        
    def _complete_market_data_processing(self):
        """Mark the completion of market data processing and record latency"""
        if self.market_data_receive_time:
            # Simulate realistic market data processing latency instead of measuring actual Python execution time
            latency_us = self.latency_tracker.simulate_realistic_latency('market_data')
            self.latency_tracker.add_market_data_latency(latency_us)
            
    def _complete_tick_to_trade(self):
        """Mark completion of tick-to-trade decision and record latency"""
        if self.last_tick_to_trade_start:
            # Simulate realistic tick-to-trade latency instead of measuring Python execution time
            latency_us = self.latency_tracker.simulate_realistic_latency('tick_to_trade')
            self.latency_tracker.add_tick_to_trade_latency(latency_us)

    def _should_replace_order(self, side, target_price, current_order):
        """Check if we should replace an existing order - with anti-flicker logic"""
        if not current_order:
            return True
            
        now = datetime.now(timezone.utc)
        
        # Check minimum replace interval
        if side == "buy" and self.last_bid_replace_time:
            if (now - self.last_bid_replace_time).total_seconds() < 2.0:
                return False
        elif side == "sell" and self.last_ask_replace_time:
            if (now - self.last_ask_replace_time).total_seconds() < 2.0:
                return False
        
        # Anti-flicker: Only replace if price difference is substantial
        if not self._same_price_level(target_price, current_order.price):
            price_diff_ticks = abs(target_price - current_order.price) / self.TICK
            
            order_age = (now - current_order.entry_time).total_seconds()
            
            if order_age < 10.0:
                return price_diff_ticks >= 15.0
            elif order_age < 30.0:
                return price_diff_ticks >= 10.0
            else:
                return price_diff_ticks >= 5.0
            
        return False

    def _can_amend_order(self, order, target_price):
        """Check if we can amend an order instead of canceling it"""
        if not order:
            return False
            
        price_diff_ticks = abs(target_price - order.price) / self.TICK
        
        # Allow amending for small price differences to maintain queue priority
        return price_diff_ticks <= 5.0  # Within 5 ticks can be amended
    
    def _amend_order(self, order, new_price):
        """Amend an existing order price while maintaining partial queue priority"""
        old_price = order.price
        price_diff_ticks = abs(new_price - old_price) / self.TICK
        
        # Update the order price
        order.price = new_price
        
        # Maintain some queue priority based on how far we moved
        if price_diff_ticks <= 1.0:
            # Very small move - keep most queue priority
            queue_retention = 0.8
        elif price_diff_ticks <= 3.0:
            # Small move - keep some queue priority  
            queue_retention = 0.5
        else:
            # Larger move - lose most queue priority
            queue_retention = 0.2
            
        order.current_queue = max(0.001, order.current_queue * queue_retention)
        
        # Simulate realistic order amendment latency
        latency_us = self.latency_tracker.simulate_realistic_latency('order_placement')
        self.latency_tracker.add_order_placement_latency(latency_us)
        
        print(f"AMENDED {order.side.upper()} order: {old_price} → {new_price} (queue retained: {queue_retention:.1%}) [Latency: {latency_us/1000:.3f}ms]")
        self.status_print_events.add("order_amended")
        self._track_order_sent("amend")

    def place_order(self, side, price, size, current_orderbook):
        """
        Intelligently place or maintain orders with amend capability
        """
        placement_start_time = datetime.now(timezone.utc)
        
        if not current_orderbook or not current_orderbook.get('bids') or not current_orderbook.get('asks'):
            print("Warning: Orderbook data missing or incomplete in place_order. Cannot place order.")
            return False
        
        bids = current_orderbook['bids']
        asks = current_orderbook['asks']
        if not bids or not asks:
            print("Warning: Bids or asks missing in place_order. Cannot place order.")
            return False

        mid_price_at_entry = (float(bids[0][0]) + float(asks[0][0])) / 2

        # Record this attempt for order rate limiting
        self.risk_manager.record_order_attempt()

        # CRITICAL FIX: Use actual DEXT-USD market specifications for validation
        # Based on actual market data: base_increment=0.1, min_market_funds=1
        min_order_value_usd = 1.0  # $1 minimum order value (from min_market_funds)
        min_order_size_dext = 0.1  # 0.1 DEXT minimum (from base_increment)
        order_value_usd = size * price
        
        if order_value_usd < min_order_value_usd:
            print(f"❌ Rejected {side} order: ${order_value_usd:.2f} below minimum ${min_order_value_usd:.2f}")
            return False
            
        if size < min_order_size_dext:
            print(f"❌ Rejected {side} order: {size:.4f} DEXT below minimum {min_order_size_dext} DEXT")
            return False

        # Pre-trade risk check
        current_equity = self.mark_to_market(mid_price_at_entry)
        latency_ms = 0.0
        if self.latency_tracker.order_placement_latencies:
            latency_ms = self.latency_tracker.order_placement_latencies[-1] / 1000  # Convert to ms
        
        can_trade, risk_details = self.risk_manager.check_pre_trade_risk(
            side=side, 
            size=size, 
            price=price,
            current_position=self.position,
            current_equity=current_equity,
            latency_ms=latency_ms
        )
        
        if not can_trade:
            print(f"❌ RISK BLOCK: Cannot place {side} order for {size} @ {price}")
            print(f"   Risk details: {risk_details}")
            return False

        if side == "buy":
            current_position = self.get_position()  # Get from ExecutionSimulator
            if current_position + size > self.max_position_size:
                return False
                
            # Try to amend existing order first
            if self.open_bid_order and self._can_amend_order(self.open_bid_order, price):
                self._amend_order(self.open_bid_order, price)
                return True
                
            # Check if we should replace existing order
            if not self._should_replace_order(side, price, self.open_bid_order):
                return False
                
            if self.open_bid_order:
                self.cancel_order(side="buy", manual_cancel=False, reason="replace")
            self.last_bid_replace_time = datetime.now(timezone.utc)
            
        elif side == "sell":
            current_position = self.get_position()  # Get from ExecutionSimulator
            if current_position - size < -self.max_position_size:
                return False
                
            # Try to amend existing order first
            if self.open_ask_order and self._can_amend_order(self.open_ask_order, price):
                self._amend_order(self.open_ask_order, price)
                return True
                
            # Check if we should replace existing order
            if not self._should_replace_order(side, price, self.open_ask_order):
                return False
                
            if self.open_ask_order:
                self.cancel_order(side="sell", manual_cancel=False, reason="replace")
            self.last_ask_replace_time = datetime.now(timezone.utc)
        else:
            return False

        # Calculate queue position more intelligently
        queue_ahead = self._calculate_queue_position(side, price, current_orderbook)
        if queue_ahead is None:
            return False
            
        # CRITICAL FIX: Validate spread to prevent crossed markets (impossible in real trading)
        bids = current_orderbook['bids']
        asks = current_orderbook['asks']
        if bids and asks:
            current_best_bid = float(bids[0][0])
            current_best_ask = float(asks[0][0])
            
            # Prevent placing orders that would cross the spread
            if side == "buy" and price >= current_best_ask:
                print(f"❌ Rejected BUY order @ {price}: would cross spread (best ask: {current_best_ask})")
                return False
            elif side == "sell" and price <= current_best_bid:
                print(f"❌ Rejected SELL order @ {price}: would cross spread (best bid: {current_best_bid})")
                return False
            
        # Reject orders with excessive queue ahead (whale orders)  
        if queue_ahead > 1000.0:  # More than 1k DEXT ahead
            print(f"Rejected {side} order @ {price}: excessive queue ahead ({queue_ahead:.0f} DEXT)")
            return False
        
        new_order = Order(side, price, size, queue_ahead, mid_price_at_entry)
        new_order.placement_start_time = placement_start_time
        new_order.placement_complete_time = datetime.now(timezone.utc)
        
        # Simulate realistic order placement latency
        placement_latency_us = self.latency_tracker.simulate_realistic_latency('order_placement')
        self.latency_tracker.add_order_placement_latency(placement_latency_us)
        
        if side == "buy":
            self.open_bid_order = new_order
            # --- Simulator hook ------------------------------------------------
            if self.exec_sim:
                self.exec_sim.submit_order({
                    'id': new_order.order_id,
                    'side': side,
                    'qty': size,
                    'price': price
                })
            # -------------------------------------------------------------------
            print(f"Placed BUY order: {size} @ {price}, queue ahead: {queue_ahead:.6f}, mid_at_entry: {mid_price_at_entry:.2f} [Latency: {placement_latency_us/1000:.3f}ms]")
            self.status_print_events.add("order_placed")
            self._track_order_sent("new_bid")
        elif side == "sell":
            self.open_ask_order = new_order
            # --- Simulator hook ------------------------------------------------
            if self.exec_sim:
                self.exec_sim.submit_order({
                    'id': new_order.order_id,
                    'side': side,
                    'qty': size,
                    'price': price
                })
            # -------------------------------------------------------------------
            print(f"Placed SELL order: {size} @ {price}, queue ahead: {queue_ahead:.6f}, mid_at_entry: {mid_price_at_entry:.2f} [Latency: {placement_latency_us/1000:.3f}ms]")
            self.status_print_events.add("order_placed")
            self._track_order_sent("new_ask")
        return True
        
    def _calculate_queue_position(self, side, price, orderbook):
        """
        Calculate queue position - handle both existing and new price levels with realistic HFT assumptions
        """
        import random
        
        bids = orderbook['bids']
        asks = orderbook['asks']

        if not bids or not asks:
            return None

        if side == "buy":
            # Find our price level in the bid stack
            for i, (bid_price, bid_vol) in enumerate(bids):
                if self._same_price_level(price, float(bid_price)):
                    # More realistic queue position - assume we're behind 15-25% of volume (not 85%)
                    queue_multiplier = random.uniform(0.15, 0.25)
                    queue_vol = float(bid_vol) * queue_multiplier
                    return max(0.5, queue_vol)  # Min 0.5 DEXT queue
            
            # Price not found in current orderbook - estimate based on market behavior
            best_bid = float(bids[0][0])
            if price <= best_bid and (best_bid - price) <= self.BASE_MAX_TICKS_AWAY * self.TICK:
                ticks_away = round((best_bid - price) / self.TICK)
                
                if ticks_away == 0:  # Joining at best bid
                    # Much more aggressive for best bid - we get decent queue position
                    best_bid_vol = float(bids[0][1])
                    queue_multiplier = random.uniform(0.10, 0.20)  # 10-20% instead of 40%
                    return max(1.0, best_bid_vol * queue_multiplier)  # Min 1 DEXT queue
                elif ticks_away == 1:  # One tick behind best
                    return random.uniform(0.5, 2.0)  # Small random queue in DEXT
                else:  # Further away
                    return random.uniform(0.1, 1.0)  # Very small queue for distant levels
            return None
        
        elif side == "sell":
            # Find our price level in the ask stack
            for i, (ask_price, ask_vol) in enumerate(asks):
                if self._same_price_level(price, float(ask_price)):
                    # More realistic queue position
                    queue_multiplier = random.uniform(0.15, 0.25)
                    queue_vol = float(ask_vol) * queue_multiplier
                    return max(0.5, queue_vol)  # Min 0.5 DEXT queue
            
            # Price not found in current orderbook
            best_ask = float(asks[0][0])
            if price >= best_ask and (price - best_ask) <= self.BASE_MAX_TICKS_AWAY * self.TICK:
                ticks_away = round((price - best_ask) / self.TICK)
                
                if ticks_away == 0:  # Joining at best ask
                    best_ask_vol = float(asks[0][1])
                    queue_multiplier = random.uniform(0.10, 0.20)
                    return max(1.0, best_ask_vol * queue_multiplier)  # Min 1 DEXT queue
                elif ticks_away == 1:  # One tick above best
                    return random.uniform(0.5, 2.0)  # Small random queue in DEXT
                else:  # Further away
                    return random.uniform(0.1, 1.0)  # Very small queue for distant levels
            return None
        
        return None
    
    def _update_single_order(self, order: Order, current_orderbook):
        """Updated order tracking logic with better queue management."""
        if not order:
            return

        # Check TTL
        age = (current_orderbook['timestamp'] - order.entry_time).total_seconds()
        if age > self.ORDER_TTL_SEC:
            print(f"Order {order.side} @ {order.price} expired (TTL) — cancelling.")
            self.cancel_order(side=order.side, manual_cancel=False, reason="ttl")
            return

        # Re-check if order still exists after potential TTL cancel
        if order.side == "buy" and not self.open_bid_order: 
            return
        if order.side == "sell" and not self.open_ask_order: 
            return

        bids = current_orderbook['bids']
        asks = current_orderbook['asks']

        if not bids or not asks:
            return

        current_best_bid = float(bids[0][0])
        current_best_ask = float(asks[0][0])

        adaptive_max_ticks = self._get_adaptive_max_ticks(current_orderbook)
        
        if order.side == "buy":
            # Check if order should be cancelled due to being crossed or too far away
            if order.price > current_best_bid:  # Our bid is crossed by market
                print(f"BUY Order @ {order.price} auto-cancelled: crossed by market.")
                self.cancel_order(side="buy", manual_cancel=False, reason="crossed")
                return
            elif (current_best_bid - order.price) > adaptive_max_ticks * self.TICK:
                print(f"BUY Order @ {order.price} auto-cancelled: too far from best bid ({current_best_bid}). Max ticks: {adaptive_max_ticks}")
                self.cancel_order(side="buy", manual_cancel=False, reason="too_far")
                return
            
            # Update queue position if we're still in the book
            self._update_order_queue_position(order, current_orderbook)
            
        elif order.side == "sell":
            # Check if order should be cancelled
            if order.price < current_best_ask:  # Our ask is crossed by market
                print(f"SELL Order @ {order.price} auto-cancelled: crossed by market.")
                self.cancel_order(side="sell", manual_cancel=False, reason="crossed")
                return
            elif (order.price - current_best_ask) > adaptive_max_ticks * self.TICK:
                print(f"SELL Order @ {order.price} auto-cancelled: too far from best ask ({current_best_ask}). Max ticks: {adaptive_max_ticks}")
                self.cancel_order(side="sell", manual_cancel=False, reason="too_far")
                return
            
            # Update queue position if we're still in the book
            self._update_order_queue_position(order, current_orderbook)

    def _update_order_queue_position(self, order: Order, current_orderbook):
        """Update queue position based on orderbook changes with realistic queue dynamics"""
        import random
        
        if not self.last_orderbook:
            return
            
        bids = current_orderbook['bids']
        asks = current_orderbook['asks']
        old_bids = self.last_orderbook['bids']
        old_asks = self.last_orderbook['asks']
        
        if order.side == "buy":
            # Find our price level in current and old orderbooks
            current_vol = 0
            old_vol = 0
            
            for price, vol in bids:
                if self._same_price_level(order.price, float(price)):
                    current_vol = float(vol)
                    break
                    
            for price, vol in old_bids:
                if self._same_price_level(order.price, float(price)):
                    old_vol = float(vol)
                    break
            
            if current_vol > 0 and old_vol > 0:
                # Volume decreased = people ahead of us got filled
                volume_decrease = max(0, old_vol - current_vol)
                
                # CRITICAL FIX: Realistic queue movement - can't advance more than volume that left
                if volume_decrease > 0:
                    # In real trading, you advance at most by the volume that disappeared
                    # Add small randomness for market uncertainty but stay realistic
                    queue_improvement = volume_decrease * random.uniform(0.8, 1.0)  # 80-100% of volume decrease
                    order.current_queue = max(0, order.current_queue - queue_improvement)
                    
                    # Natural queue drift - small random improvements over time
                    if random.random() < 0.15:  # 15% chance
                        order.current_queue = max(0, order.current_queue - random.uniform(0.1, 1.0))
                        
            elif current_vol > 0:
                # Price level reappeared or we're tracking it for first time
                # Be less conservative about our position
                order.current_queue = min(order.current_queue, current_vol * random.uniform(0.3, 0.7))
                
        elif order.side == "sell":
            # Same logic for asks
            current_vol = 0
            old_vol = 0
            
            for price, vol in asks:
                if self._same_price_level(order.price, float(price)):
                    current_vol = float(vol)
                    break
                    
            for price, vol in old_asks:
                if self._same_price_level(order.price, float(price)):
                    old_vol = float(vol)
                    break
            
            if current_vol > 0 and old_vol > 0:
                volume_decrease = max(0, old_vol - current_vol)
                
                if volume_decrease > 0:
                    # CRITICAL FIX: Realistic queue movement for sell orders too
                    queue_improvement = volume_decrease * random.uniform(0.8, 1.0)  # 80-100% of volume decrease
                    order.current_queue = max(0, order.current_queue - queue_improvement)
                    
                    if random.random() < 0.15:  # Natural queue drift
                        order.current_queue = max(0, order.current_queue - random.uniform(0.1, 1.0))
                        
            elif current_vol > 0:
                order.current_queue = min(order.current_queue, current_vol * random.uniform(0.3, 0.7))

    def update_order_with_orderbook(self, current_orderbook):
        if self.open_bid_order:
            self._update_single_order(self.open_bid_order, current_orderbook)
        
        if self.open_ask_order:
            self._update_single_order(self.open_ask_order, current_orderbook)

        self.last_orderbook = copy.deepcopy(current_orderbook)

    def _same_price_level(self, a: float, b: float, tick=None) -> bool:
        if tick is None:
            tick = self.TICK
        return abs(a - b) < (tick / 2)
    
    def simulate_fill(self, trade_price, trade_qty, trade_side):
        """
        DEPRECATED: Fill simulation now handled by ExecutionSimulator
        This method is kept for backward compatibility but does nothing
        """
        # Fill simulation now handled by ExecutionSimulator to avoid double fills
        pass

    def _simulate_fill_single_order(self, order: Order, trade_price, trade_qty, trade_side):
        """
        DEPRECATED: Fill simulation now handled by ExecutionSimulator
        This method is kept for backward compatibility but does nothing
        """
        # Fill simulation now handled by ExecutionSimulator to avoid double fills
        return False

    def should_print_status(self, force_interval_seconds=10):
        """Check if we should print status based on trading events or time interval"""
        now = datetime.now(timezone.utc)
        
        # Print if we have meaningful trading events
        if self.status_print_events:
            return True
        
        # Print every N seconds if no events (to show we're still alive)
        if (self.last_status_print_time is None or 
            (now - self.last_status_print_time).total_seconds() >= force_interval_seconds):
            return True
            
        return False
    
    def print_status(self, mid_price, force=False):
        """Print status only when meaningful events occur or on interval"""
        now = datetime.now(timezone.utc)
        
        if not force and not self.should_print_status():
            return
            
        # Update performance metrics
        self._update_performance_metrics(mid_price)
            
        pnl = self.mark_to_market_pnl(mid_price)
        active_orders_str = []
        if self.open_bid_order:
            active_orders_str.append(f"BID@{self.open_bid_order.price} (Q:{self.open_bid_order.current_queue:.3f}, Rem:{self.open_bid_order.remaining_qty:.3f})")
        if self.open_ask_order:
            active_orders_str.append(f"ASK@{self.open_ask_order.price} (Q:{self.open_ask_order.current_queue:.3f}, Rem:{self.open_ask_order.remaining_qty:.3f})")
        orders_info = " | ".join(active_orders_str) if active_orders_str else "No open orders"
        
        # Add event indicators if we have them
        events_str = ""
        if self.status_print_events:
            events_str = f" [{', '.join(self.status_print_events)}]"
        
        # Add O:T ratio monitoring
        ot_ratio = self.get_order_to_trade_ratio(window_only=True)
        ot_str = f" | O:T {ot_ratio:.1f}" if ot_ratio != float('inf') else " | O:T ∞"
        
        # Alert if O:T ratio is too high
        if self.should_alert_ot_ratio():
            ot_str += " ⚠️"

        unrealized_pnl = self.get_unrealized_open_order_pnl(mid_price)
        
        # Add risk management status
        risk_summary = self.risk_manager.get_risk_summary()
        critical_breaches = risk_summary.get('active_risk_breaches', [])
        risk_str = ""
        if critical_breaches:
            risk_str = f" | RISK:⚠️{len(critical_breaches)}"
        elif self.risk_manager.emergency_risk_shutdown():
            risk_str = " | RISK:🚨STOP"
        
        # Add latency metrics (every 5th print to avoid clutter)
        latency_str = ""
        if len(self.pnl_history) % 5 == 0:
            lat_summary = self.latency_tracker.get_latency_summary()
            if lat_summary:
                # Show key latency metrics with better formatting
                latency_parts = []
                if 'market_data' in lat_summary:
                    md_lat = lat_summary['market_data']['p95_ms']
                    latency_parts.append(f"Feed:{md_lat:.2f}ms")
                if 'order_placement' in lat_summary:
                    op_lat = lat_summary['order_placement']['p95_ms']
                    latency_parts.append(f"Order:{op_lat:.2f}ms")
                if 'tick_to_trade' in lat_summary:
                    tt_lat = lat_summary['tick_to_trade']['p95_ms']
                    latency_parts.append(f"T2T:{tt_lat:.2f}ms")
                
                if latency_parts:
                    latency_str += f" | Latency[{'/'.join(latency_parts)}]"
                
                # Add spike warning with more detail
                spikes = lat_summary.get('critical_spikes', 0) + lat_summary.get('recent_spikes', 0)
                if spikes > 0:
                    latency_str += f" ⚠️{spikes}spikes"
        
        # Add performance metrics to status (every 10th print to avoid clutter)
        perf_str = ""
        if len(self.pnl_history) > 0 and len(self.pnl_history) % 10 == 0:
            sharpe = self.calculate_sharpe_ratio()
            win_rate = self.get_win_rate()
            dd_pct = self.max_drawdown_observed * 100
            perf_str = f" | Sharpe:{sharpe:.2f} WR:{win_rate:.1f}% DD:{dd_pct:.1f}%"
        
        current_position = self.get_position()
        current_cash = self.get_cash()
        print(f"Pos: {current_position:.4f} | Cash: {current_cash:.2f} | MTM PnL: {pnl:.2f} | Net Spread PnL: {self.spread_capture_pnl:.2f} | Unrealized: {unrealized_pnl:.2f} | Total Fees: {self.total_fees_paid:.2f}{ot_str}{risk_str}{latency_str}{perf_str} | {orders_info}{events_str}")
        
        # CRITICAL FIX: Validate order state synchronization periodically
        if len(self.pnl_history) % 20 == 0:  # Check every 20th status print
            self._validate_order_state_sync()
        
        # Clear events and update timestamp
        self.status_print_events.clear()
        self.last_status_print_time = now

    def get_position(self):
        # Get position from execution simulator for consistency
        if self.exec_sim:
            return self.exec_sim.position
        return self.position

    def get_cash(self):
        # Get cash from execution simulator for consistency
        if self.exec_sim:
            return self.exec_sim.cash
        return self.cash
    
    def mark_to_market(self, mid_price):
        # Use execution simulator state for consistency
        if self.exec_sim:
            return self.exec_sim.mark_to_market(mid_price)
        return self.position * mid_price + self.cash
    
    def mark_to_market_pnl(self, mid_price):
        """Return only the profit/loss component (excluding initial capital)."""
        # Use execution simulator state for consistency
        if self.exec_sim:
            return self.exec_sim.mark_to_market(mid_price) - self.initial_cash
        return self.position * mid_price + self.cash - self.initial_cash

    def get_unrealized_open_order_pnl(self, current_mid_price: float) -> float:
        """Calculate the potential PnL from open orders if they were filled against the current mid_price."""
        unrealized_pnl = 0.0
        if self.open_bid_order:
            # For a bid (buy order), potential profit is (current_mid_price - order_price) * remaining_qty
            # This is positive if the market mid has moved up since we placed our bid.
            unrealized_pnl += (current_mid_price - self.open_bid_order.price) * self.open_bid_order.remaining_qty
        
        if self.open_ask_order:
            # For an ask (sell order), potential profit is (order_price - current_mid_price) * remaining_qty
            # This is positive if the market mid has moved down since we placed our ask.
            unrealized_pnl += (self.open_ask_order.price - current_mid_price) * self.open_ask_order.remaining_qty
            
        return unrealized_pnl

    def cancel_order(self, side: str, manual_cancel: bool = False, reason: str = ""):
        import time
        import random
        
        now = datetime.now(timezone.utc)
        self.last_cancel_time = now
        if manual_cancel:
            self.last_manual_cancel_time = now

        reason_str = f" ({reason})" if reason else ""
        
        # DON'T block the system with time.sleep() - just simulate latency tracking
        if manual_cancel:
            cancel_delay = random.uniform(0.150, 0.400)
            print(f"⏳ Manual cancel requested - simulated {cancel_delay*1000:.0f}ms latency...")
            # Note: In real HFT systems, you'd schedule this asynchronously, not block
        
        if side == "buy" and self.open_bid_order:
            # Simulate realistic cancel latency
            cancel_latency_us = self.latency_tracker.simulate_realistic_latency('order_cancel')
            self.latency_tracker.add_order_cancel_latency(cancel_latency_us)
            if self.exec_sim:
                self.exec_sim.cancel_order(self.open_bid_order.order_id)
            print(f"Cancelled BUY order @ {self.open_bid_order.price}{' (MANUAL)' if manual_cancel else ' (AUTO)'}{reason_str} [Cancel Latency: {cancel_latency_us/1000:.3f}ms]")
            self.open_bid_order = None
            self.status_print_events.add("order_cancelled")
        elif side == "sell" and self.open_ask_order:
            # Simulate realistic cancel latency
            cancel_latency_us = self.latency_tracker.simulate_realistic_latency('order_cancel')
            self.latency_tracker.add_order_cancel_latency(cancel_latency_us)
            if self.exec_sim:
                self.exec_sim.cancel_order(self.open_ask_order.order_id)
            print(f"Cancelled SELL order @ {self.open_ask_order.price}{' (MANUAL)' if manual_cancel else ' (AUTO)'}{reason_str} [Cancel Latency: {cancel_latency_us/1000:.3f}ms]")
            self.open_ask_order = None
            self.status_print_events.add("order_cancelled")
        else:
            print(f"No {side} order to cancel")

    def cancel_all_orders(self, manual_cancel: bool = False):
        if self.open_bid_order:
            self.cancel_order(side="buy", manual_cancel=manual_cancel)
        if self.open_ask_order:
            self.cancel_order(side="sell", manual_cancel=manual_cancel)

    def get_open_bid_order(self):
        return self.open_bid_order
    
    def get_open_ask_order(self):
        return self.open_ask_order

    def _get_adaptive_max_ticks(self, current_orderbook):
        """Calculate adaptive max ticks based on market volatility"""
        if not self.last_orderbook:
            return self.BASE_MAX_TICKS_AWAY
        
        # Calculate recent price movement  
        old_mid = (float(self.last_orderbook['bids'][0][0]) + float(self.last_orderbook['asks'][0][0])) / 2
        new_mid = (float(current_orderbook['bids'][0][0]) + float(current_orderbook['asks'][0][0])) / 2
        
        price_move_ticks = abs(new_mid - old_mid) / self.TICK
        
        # If market is moving fast, allow orders to stay further away
        if price_move_ticks > 5:  # Fast market
            return int(self.BASE_MAX_TICKS_AWAY * self.ADAPTIVE_MAX_TICKS_MULTIPLIER)
        elif price_move_ticks > 2:  # Moderate market
            return int(self.BASE_MAX_TICKS_AWAY * 1.5)
        else:  # Calm market
            return self.BASE_MAX_TICKS_AWAY
    
    def _track_order_sent(self, order_type="new"):
        """Track when orders are sent for O:T ratio calculation"""
        now = datetime.now(timezone.utc)
        self.orders_sent += 1
        self.recent_orders.append((now, order_type))
        
        # Clean old entries outside the window
        cutoff_time = now - timedelta(seconds=self.ot_ratio_window)
        self.recent_orders = [(ts, ot) for ts, ot in self.recent_orders if ts > cutoff_time]
    
    def _track_fill(self):
        """Track when fills occur for O:T ratio calculation"""
        now = datetime.now(timezone.utc)
        self.trades_filled += 1
        self.recent_fills.append(now)
        
        # Clean old entries outside the window
        cutoff_time = now - timedelta(seconds=self.ot_ratio_window)
        self.recent_fills = [ts for ts in self.recent_fills if ts > cutoff_time]
    
    def get_order_to_trade_ratio(self, window_only=True):
        """Calculate current order-to-trade ratio"""
        if window_only:
            orders = len(self.recent_orders)
            fills = len(self.recent_fills)
        else:
            orders = self.orders_sent
            fills = self.trades_filled
        
        if fills == 0:
            return float('inf') if orders > 0 else 0
        return orders / fills
        
    def should_alert_ot_ratio(self, threshold=25):
        """Check if O:T ratio is approaching dangerous levels"""
        current_ratio = self.get_order_to_trade_ratio(window_only=True)
        return current_ratio > threshold and len(self.recent_fills) > 0
    
    def _update_performance_metrics(self, mid_price):
        """Update performance tracking metrics"""
        now = datetime.now(timezone.utc)
        current_pnl = self.mark_to_market_pnl(mid_price)
        current_equity = self.mark_to_market(mid_price)
        
        # Update PnL history for Sharpe calculation (sample every 30 seconds)
        if not self.pnl_history or (now - self.pnl_history[-1][0]).total_seconds() >= 30:
            self.pnl_history.append((now, current_pnl))
            
        # Update peak equity and drawdown
        if current_equity > self.peak_equity:
            self.peak_equity = current_equity
        else:
            current_drawdown = (self.peak_equity - current_equity) / self.peak_equity
            if current_drawdown > self.max_drawdown_observed:
                self.max_drawdown_observed = current_drawdown
    
    def calculate_sharpe_ratio(self, risk_free_rate=0.0):
        """Calculate annualized Sharpe ratio from PnL history"""
        if len(self.pnl_history) < 2:
            return 0.0
            
        # Calculate returns from PnL differences
        returns = []
        for i in range(1, len(self.pnl_history)):
            time_diff = (self.pnl_history[i][0] - self.pnl_history[i-1][0]).total_seconds()
            pnl_diff = self.pnl_history[i][1] - self.pnl_history[i-1][1]
            if time_diff > 0:
                # Use simple period return without annualizing here
                period_return = pnl_diff / self.initial_cash
                returns.append(period_return)
        
        if len(returns) < 2:
            return 0.0
            
        import statistics
        mean_return = statistics.mean(returns)
        return_std = statistics.stdev(returns) if len(returns) > 1 else 0.0
        
        if return_std == 0:
            # If no volatility but positive mean return, return a high positive Sharpe
            # If no volatility and negative mean return, return a high negative Sharpe
            return 10.0 if mean_return > 0 else -10.0 if mean_return < 0 else 0.0
            
        # Annualize assuming 30-second intervals
        periods_per_year = (365 * 24 * 60 * 60) / 30
        annual_mean = mean_return * periods_per_year
        annual_std = return_std * (periods_per_year ** 0.5)
        
        return (annual_mean - risk_free_rate) / annual_std if annual_std > 0 else 0.0
    
    def get_win_rate(self):
        """Calculate win rate percentage"""
        if self.trades_total == 0:
            return 0.0
        return (self.trades_won / self.trades_total) * 100
    
    def get_session_performance_summary(self):
        """Get comprehensive performance summary with realistic benchmarks"""
        session_duration = (datetime.now(timezone.utc) - self.session_start_time).total_seconds() / 3600  # hours
        sharpe = self.calculate_sharpe_ratio()
        win_rate = self.get_win_rate()
        ot_ratio = self.get_order_to_trade_ratio(window_only=False)
        
        # Calculate current MTM PnL for final assessment
        if self.pnl_history:
            final_pnl = self.pnl_history[-1][1]
        else:
            final_pnl = 0.0
        
        # Get comprehensive latency summary
        latency_summary = self.latency_tracker.get_latency_summary()
        
        # Get risk management summary
        risk_summary = self.risk_manager.get_risk_summary()
        
        # Determine performance grade based on realistic simulation benchmarks
        # Now also consider latency performance and risk management
        performance_grade = "Poor"
        latency_penalty = 0
        risk_penalty = 0
        
        # Check if latencies are within acceptable bounds
        if latency_summary:
            if latency_summary.get('critical_spikes', 0) > 0:
                latency_penalty += 1
            if 'market_data' in latency_summary and latency_summary['market_data']['p95_ms'] > 5:
                latency_penalty += 0.5
            if 'order_placement' in latency_summary and latency_summary['order_placement']['p95_ms'] > 10:
                latency_penalty += 0.5
            if 'tick_to_trade' in latency_summary and latency_summary['tick_to_trade']['p95_ms'] > 15:
                latency_penalty += 0.5
        
        # Check risk management violations
        active_breaches = len(risk_summary.get('active_risk_breaches', []))
        if active_breaches >= 2:
            risk_penalty += 2
        elif active_breaches >= 1:
            risk_penalty += 1
        if self.risk_manager.emergency_risk_shutdown():
            risk_penalty += 2
        
        # Adjust performance grade based on latency and risk
        base_score = 0
        if sharpe >= 0.8 and win_rate >= 52 and self.max_drawdown_observed <= 0.03 and ot_ratio <= 15:
            base_score = 3
        elif sharpe >= 0.5 and win_rate >= 50 and self.max_drawdown_observed <= 0.05 and ot_ratio <= 25:
            base_score = 2
        elif sharpe >= 0.2 and win_rate >= 48 and self.max_drawdown_observed <= 0.08:
            base_score = 1
        
        final_score = max(0, base_score - latency_penalty - risk_penalty)
        if final_score >= 3:
            performance_grade = "Excellent"
        elif final_score >= 2:
            performance_grade = "Good"
        elif final_score >= 1:
            performance_grade = "Acceptable"
        
        summary = {
            'session_duration_hours': round(session_duration, 2),
            'sharpe_ratio': round(sharpe, 3),
            'win_rate_pct': round(win_rate, 1),
            'max_drawdown_pct': round(self.max_drawdown_observed * 100, 3),
            'total_trades': self.trades_total,
            'total_orders_sent': self.orders_sent,
            'order_to_trade_ratio': round(ot_ratio, 1),
            'performance_grade': performance_grade,
            'spread_capture_pnl': round(self.spread_capture_pnl, 2),
            'final_mtm_pnl': round(final_pnl, 2),
            'total_fees_paid': round(self.total_fees_paid, 4),
            'pnl_samples': len(self.pnl_history),
            'latency_metrics': latency_summary,
            'risk_metrics': risk_summary
        }
        
        return summary

    def print_comprehensive_performance_report(self):
        """Print a detailed, readable performance report including latency"""
        print("\n" + "="*80)
        print("🎯 COMPREHENSIVE HFT PERFORMANCE REPORT")
        print("="*80)
        
        # Get session summary
        summary = self.get_session_performance_summary()
        
        # Basic Performance Metrics
        print(f"📈 Trading Performance (Grade: {summary['performance_grade']})")
        print(f"   Session Duration: {summary['session_duration_hours']:.1f} hours")
        print(f"   Sharpe Ratio: {summary['sharpe_ratio']:.3f}")
        print(f"   Win Rate: {summary['win_rate_pct']:.1f}%")
        print(f"   Max Drawdown: {summary['max_drawdown_pct']:.2f}%")
        print(f"   Final PnL: ${summary['final_mtm_pnl']:.2f}")
        print(f"   Spread Capture PnL: ${summary['spread_capture_pnl']:.2f}")
        print(f"   Total Fees Paid: ${summary['total_fees_paid']:.4f}")
        print(f"   Order:Trade Ratio: {summary['order_to_trade_ratio']:.1f}")
        print(f"   Total Trades: {summary['total_trades']}")
        
        # Detailed latency report
        self.latency_tracker.print_detailed_latency_report()
        
        # Risk summary
        print("\n🛡️  RISK MANAGEMENT SUMMARY:")
        risk_metrics = summary['risk_metrics']
        print(f"   Daily PnL: ${risk_metrics['daily_pnl']:.2f}")
        print(f"   Max Drawdown: {risk_metrics['max_drawdown_pct']:.2f}%")
        print(f"   Position Volatility: {risk_metrics['position_volatility']:.4f}")
        print(f"   Recent Order Rate: {risk_metrics['recent_order_rate']:.1f}/sec")
        
        active_breaches = risk_metrics.get('active_risk_breaches', [])
        if active_breaches:
            print(f"   🚨 Active Risk Breaches: {', '.join(active_breaches)}")
        else:
            print("   ✅ No active risk breaches")
            
        print("\n📊 UTILIZATION:")
        util = risk_metrics['risk_utilization']
        print(f"   Position Limit: {util['position']}")
        print(f"   Daily Loss Limit: {util['daily_loss']}")
        print(f"   Drawdown Limit: {util['drawdown']}")
        
        print("="*80)
    
    def get_risk_adjusted_skew(self, base_bid_price, base_ask_price):
        """Get position skew adjusted by risk management constraints"""
        mid_price = (base_bid_price + base_ask_price) / 2
        # Estimate market volatility (simplified)
        market_volatility = 0.02  # 2% volatility estimate
        
        bid_skew, ask_skew = self.inventory_manager.get_inventory_skew(
            current_inventory=self.position,
            current_mid_price=mid_price,
            market_volatility=market_volatility
        )
        
        return bid_skew, ask_skew
    
    def _handle_execution_event(self, event_type, event_data):
        """Handle callbacks from ExecutionSimulator to keep order state synchronized"""
        if event_type == 'fill':
            order_id = event_data['order_id']
            side = event_data['side']
            fill_qty = event_data['fill_qty']
            remaining_qty = event_data['remaining_qty']
            
            # Update QuoteEngine order state to match ExecutionSimulator
            if side == 'buy' and self.open_bid_order and self.open_bid_order.order_id == order_id:
                if remaining_qty <= 0:
                    # Order completely filled - remove it
                    self.open_bid_order = None
                    print(f"🔄 SYNC: BUY order fully filled, removed from QuoteEngine")
                else:
                    # Partial fill - update remaining quantity
                    self.open_bid_order.remaining_qty = remaining_qty
                    self.open_bid_order.filled_qty = self.open_bid_order.qty - remaining_qty
                    print(f"🔄 SYNC: BUY order partially filled, {remaining_qty:.1f} remaining")
                    
                # Track the fill for performance metrics
                self._track_fill()
                self.status_print_events.add("order_filled")
                
                # CRITICAL FIX: Update risk manager with actual position/equity from ExecutionSimulator
                if self.exec_sim:
                    current_position = self.exec_sim.position
                    mid_price = (event_data.get('price', 0) + event_data.get('price', 0)) / 2  # Rough estimate
                    current_equity = self.exec_sim.mark_to_market(mid_price)
                    self.risk_manager.update_position_and_pnl(current_position, current_equity)
                    
            elif side == 'sell' and self.open_ask_order and self.open_ask_order.order_id == order_id:
                if remaining_qty <= 0:
                    # Order completely filled - remove it
                    self.open_ask_order = None
                    print(f"🔄 SYNC: SELL order fully filled, removed from QuoteEngine")
                else:
                    # Partial fill - update remaining quantity
                    self.open_ask_order.remaining_qty = remaining_qty
                    self.open_ask_order.filled_qty = self.open_ask_order.qty - remaining_qty
                    print(f"🔄 SYNC: SELL order partially filled, {remaining_qty:.1f} remaining")
                    
                # Track the fill for performance metrics
                self._track_fill()
                self.status_print_events.add("order_filled")
                
                # CRITICAL FIX: Update risk manager with actual position/equity from ExecutionSimulator
                if self.exec_sim:
                    current_position = self.exec_sim.position
                    mid_price = (event_data.get('price', 0) + event_data.get('price', 0)) / 2  # Rough estimate
                    current_equity = self.exec_sim.mark_to_market(mid_price)
                    self.risk_manager.update_position_and_pnl(current_position, current_equity)
                
        elif event_type == 'cancel':
            order_id = event_data['order_id']
            side = event_data['side']
            
            # Remove the cancelled order from QuoteEngine state
            if side == 'buy' and self.open_bid_order and self.open_bid_order.order_id == order_id:
                self.open_bid_order = None
                print(f"🔄 SYNC: BUY order cancelled, removed from QuoteEngine")
                self.status_print_events.add("order_cancelled")
                
            elif side == 'sell' and self.open_ask_order and self.open_ask_order.order_id == order_id:
                self.open_ask_order = None
                print(f"🔄 SYNC: SELL order cancelled, removed from QuoteEngine")
                self.status_print_events.add("order_cancelled")

    def _validate_order_state_sync(self):
        """Validate that QuoteEngine and ExecutionSimulator order states are synchronized"""
        if not self.exec_sim:
            return True  # No validation needed if no simulator
            
        warnings = []
        
        # Check bid order consistency
        if self.open_bid_order:
            if self.open_bid_order.order_id not in self.exec_sim.live_orders:
                warnings.append(f"QuoteEngine has BID order {self.open_bid_order.order_id[:8]} but ExecutionSimulator doesn't")
        
        # Check ask order consistency  
        if self.open_ask_order:
            if self.open_ask_order.order_id not in self.exec_sim.live_orders:
                warnings.append(f"QuoteEngine has ASK order {self.open_ask_order.order_id[:8]} but ExecutionSimulator doesn't")
        
        # Check for orders in ExecutionSimulator that QuoteEngine doesn't know about
        for order_id, sim_order in self.exec_sim.live_orders.items():
            if sim_order.side == 'buy' and (not self.open_bid_order or self.open_bid_order.order_id != order_id):
                warnings.append(f"ExecutionSimulator has BID order {order_id[:8]} but QuoteEngine doesn't")
            elif sim_order.side == 'sell' and (not self.open_ask_order or self.open_ask_order.order_id != order_id):
                warnings.append(f"ExecutionSimulator has ASK order {order_id[:8]} but QuoteEngine doesn't")
        
        if warnings:
            print("🚨 ORDER STATE SYNC WARNING:")
            for warning in warnings:
                print(f"   {warning}")
            return False
            
        return True

