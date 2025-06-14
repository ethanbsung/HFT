from datetime import datetime, timezone, timedelta
import copy
from collections import deque
import statistics
from risk_manager import RiskManager, RiskLimits, InventoryManager

class LatencyTracker:
    """Track various latency metrics for HFT performance monitoring"""
    
    def __init__(self, window_size=1000):
        self.window_size = window_size
        
        # Rolling windows for different latency types (in microseconds)
        self.order_to_fill_latencies = deque(maxlen=window_size)
        self.market_data_processing_latencies = deque(maxlen=window_size)
        self.order_placement_latencies = deque(maxlen=window_size)
        self.order_cancel_latencies = deque(maxlen=window_size)
        self.tick_to_trade_latencies = deque(maxlen=window_size)
        
        # Latency spike tracking
        self.latency_spikes = deque(maxlen=100)  # Keep last 100 spikes
        
        # Thresholds (in microseconds)
        self.thresholds = {
            'order_to_fill_warning': 50000,      # 50ms
            'order_to_fill_critical': 100000,    # 100ms
            'market_data_warning': 5000,          # 5ms
            'market_data_critical': 10000,        # 10ms
            'order_placement_warning': 10000,     # 10ms
            'order_placement_critical': 25000,    # 25ms
            'tick_to_trade_warning': 15000,       # 15ms
            'tick_to_trade_critical': 30000       # 30ms
        }
        
        # Current session tracking
        self.session_start = datetime.now(timezone.utc)
        self.last_latency_report_time = None
        
    def add_order_to_fill_latency(self, latency_us: float):
        """Add order-to-fill latency measurement"""
        self.order_to_fill_latencies.append(latency_us)
        self._check_spike('order_to_fill', latency_us)
        
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
        if latency_type == 'order_to_fill':
            data = list(self.order_to_fill_latencies)
        elif latency_type == 'market_data':
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
        latency_types = ['order_to_fill', 'market_data', 'order_placement', 'tick_to_trade']
        
        for lat_type in latency_types:
            stats = self.get_statistics(lat_type)
            if stats:
                summary[lat_type] = {
                    'mean_ms': round(stats['mean_us'] / 1000, 2),
                    'p95_ms': round(stats['p95_us'] / 1000, 2),
                    'p99_ms': round(stats['p99_us'] / 1000, 2),
                    'max_ms': round(stats['max_us'] / 1000, 2),
                    'count': stats['count']
                }
        
        # Add spike information
        recent_spikes = self.get_recent_spikes(minutes=5)
        summary['recent_spikes'] = len(recent_spikes)
        summary['critical_spikes'] = len([s for s in recent_spikes if s['severity'] == 'critical'])
        
        return summary

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
    TICK = 0.01
    BASE_MAX_TICKS_AWAY = 15
    ADAPTIVE_MAX_TICKS_MULTIPLIER = 2.0
    ORDER_TTL_SEC = 120.0
    MIN_ORDER_REPLACE_INTERVAL = 0.5
    MAKER_FEE_RATE = 0.0000

    def __init__(self, max_position_size=0.05):
        self.position = 0
        self.cash = 100_000.0
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

    def _start_market_data_processing(self):
        """Mark the start of market data processing"""
        self.market_data_receive_time = datetime.now(timezone.utc)
        self.last_tick_to_trade_start = self.market_data_receive_time
        
    def _complete_market_data_processing(self):
        """Mark the completion of market data processing and record latency"""
        if self.market_data_receive_time:
            complete_time = datetime.now(timezone.utc)
            latency_us = (complete_time - self.market_data_receive_time).total_seconds() * 1_000_000
            self.latency_tracker.add_market_data_latency(latency_us)
            
    def _complete_tick_to_trade(self):
        """Mark completion of tick-to-trade decision and record latency"""
        if self.last_tick_to_trade_start:
            complete_time = datetime.now(timezone.utc)
            latency_us = (complete_time - self.last_tick_to_trade_start).total_seconds() * 1_000_000
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
        start_time = datetime.now(timezone.utc)
        
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
        
        # Record latency for order amendment
        complete_time = datetime.now(timezone.utc)
        latency_us = (complete_time - start_time).total_seconds() * 1_000_000
        self.latency_tracker.add_order_placement_latency(latency_us)
        
        print(f"AMENDED {order.side.upper()} order: {old_price} → {new_price} (queue retained: {queue_retention:.1%}) [Latency: {latency_us/1000:.2f}ms]")
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
            if self.position + size > self.max_position_size:
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
            if self.position - size < -self.max_position_size:
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
            
        # Reject orders with excessive queue ahead (whale orders)
        if queue_ahead > 5.0:  # More than 5 BTC ahead
            print(f"Rejected {side} order @ {price}: excessive queue ahead ({queue_ahead:.2f} BTC)")
            return False
        
        new_order = Order(side, price, size, queue_ahead, mid_price_at_entry)
        new_order.placement_start_time = placement_start_time
        new_order.placement_complete_time = datetime.now(timezone.utc)
        
        # Record order placement latency
        placement_latency_us = (new_order.placement_complete_time - placement_start_time).total_seconds() * 1_000_000
        self.latency_tracker.add_order_placement_latency(placement_latency_us)
        
        if side == "buy":
            self.open_bid_order = new_order
            print(f"Placed BUY order: {size} @ {price}, queue ahead: {queue_ahead:.6f}, mid_at_entry: {mid_price_at_entry:.2f} [Latency: {placement_latency_us/1000:.2f}ms]")
            self.status_print_events.add("order_placed")
            self._track_order_sent("new_bid")
        elif side == "sell":
            self.open_ask_order = new_order
            print(f"Placed SELL order: {size} @ {price}, queue ahead: {queue_ahead:.6f}, mid_at_entry: {mid_price_at_entry:.2f} [Latency: {placement_latency_us/1000:.2f}ms]")
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
                    return max(0.002, queue_vol)  # Much smaller minimum queue
            
            # Price not found in current orderbook - estimate based on market behavior
            best_bid = float(bids[0][0])
            if price <= best_bid and (best_bid - price) <= self.BASE_MAX_TICKS_AWAY * self.TICK:
                ticks_away = round((best_bid - price) / self.TICK)
                
                if ticks_away == 0:  # Joining at best bid
                    # Much more aggressive for best bid - we get decent queue position
                    best_bid_vol = float(bids[0][1])
                    queue_multiplier = random.uniform(0.10, 0.20)  # 10-20% instead of 40%
                    return max(0.005, best_bid_vol * queue_multiplier)
                elif ticks_away == 1:  # One tick behind best
                    return random.uniform(0.002, 0.008)  # Small random queue
                else:  # Further away
                    return random.uniform(0.001, 0.003)  # Very small queue for distant levels
            return None
        
        elif side == "sell":
            # Find our price level in the ask stack
            for i, (ask_price, ask_vol) in enumerate(asks):
                if self._same_price_level(price, float(ask_price)):
                    # More realistic queue position
                    queue_multiplier = random.uniform(0.15, 0.25)
                    queue_vol = float(ask_vol) * queue_multiplier
                    return max(0.002, queue_vol)
            
            # Price not found in current orderbook
            best_ask = float(asks[0][0])
            if price >= best_ask and (price - best_ask) <= self.BASE_MAX_TICKS_AWAY * self.TICK:
                ticks_away = round((price - best_ask) / self.TICK)
                
                if ticks_away == 0:  # Joining at best ask
                    best_ask_vol = float(asks[0][1])
                    queue_multiplier = random.uniform(0.10, 0.20)
                    return max(0.005, best_ask_vol * queue_multiplier)
                elif ticks_away == 1:  # One tick above best
                    return random.uniform(0.002, 0.008)
                else:  # Further away
                    return random.uniform(0.001, 0.003)
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
                
                # Enhanced queue movement - sometimes we move up more than volume decrease
                if volume_decrease > 0:
                    # Add some randomness to queue improvement
                    queue_improvement = volume_decrease * random.uniform(1.0, 1.5)
                    order.current_queue = max(0, order.current_queue - queue_improvement)
                    
                    # Natural queue drift - small random improvements over time
                    if random.random() < 0.15:  # 15% chance
                        order.current_queue = max(0, order.current_queue - random.uniform(0.001, 0.005))
                        
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
                    queue_improvement = volume_decrease * random.uniform(1.0, 1.5)
                    order.current_queue = max(0, order.current_queue - queue_improvement)
                    
                    if random.random() < 0.15:  # Natural queue drift
                        order.current_queue = max(0, order.current_queue - random.uniform(0.001, 0.005))
                        
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
    
    def _simulate_fill_single_order(self, order: Order, trade_price, trade_qty, trade_side):
        import random
        
        if not order:
            return False
        
        if not self._same_price_level(trade_price, order.price):
            return False
        
        if order.side == "buy" and trade_side != "sell":
            return False
        if order.side == "sell" and trade_side != "buy":
            return False
        
        print(f"Trade hits our {order.side.upper()} order level: {trade_side} {trade_qty} @ {trade_price}")
        print(f"Queue ahead for {order.side.upper()} order: {order.current_queue:.6f}")

        # More aggressive queue reduction for small orders near front
        if order.current_queue < 0.05:  # Very close to front of queue
            # Accelerated queue movement when we're near the front
            queue_reduction = trade_qty * random.uniform(1.2, 2.0)  # 20-100% bonus reduction
        else:
            queue_reduction = trade_qty

        order.current_queue -= queue_reduction

        cq_before_trade_impact = order.current_queue + queue_reduction
        if cq_before_trade_impact <= 0: 
            cq_before_trade_impact = 0

        # Enhanced fill logic with probabilistic fills
        our_fill = 0
        
        if order.current_queue <= 0:
            # Standard fill logic when queue is cleared
            qty_past_our_turn = queue_reduction - cq_before_trade_impact
            if qty_past_our_turn < 0: 
                qty_past_our_turn = 0
            our_fill = min(order.remaining_qty, qty_past_our_turn)
        else:
            # Probabilistic fill for orders very close to front of queue
            if order.current_queue < 0.01:  # Within 0.01 BTC of front
                fill_probability = min(0.85, (trade_qty / 0.02) * (0.01 / max(0.001, order.current_queue)))
                if random.random() < fill_probability:
                    # Partial fill based on trade size and proximity to front
                    max_fill = min(order.remaining_qty, trade_qty * random.uniform(0.3, 0.8))
                    our_fill = max_fill
                    order.current_queue = 0  # Move to front after partial fill
            elif order.current_queue < 0.05:  # Close to front
                fill_probability = min(0.35, trade_qty / 0.05)
                if random.random() < fill_probability:
                    our_fill = min(order.remaining_qty, trade_qty * random.uniform(0.1, 0.4))
                    order.current_queue = max(0, order.current_queue - our_fill)

        if our_fill > 0.0000001:
                fill_time = datetime.now(timezone.utc)
                
                # Calculate order-to-fill latency
                if order.placement_complete_time:
                    order_to_fill_latency_us = (fill_time - order.placement_complete_time).total_seconds() * 1_000_000
                    self.latency_tracker.add_order_to_fill_latency(order_to_fill_latency_us)
                
                order.filled_qty += our_fill
                order.remaining_qty -= our_fill

                if order.side == "buy":
                    self.position += our_fill
                    self.cash -= order.price * our_fill
                    # Calculate spread capture PnL
                    trade_value = order.price * our_fill
                    fee_for_fill = trade_value * self.MAKER_FEE_RATE
                    self.cash -= fee_for_fill
                    self.total_fees_paid += fee_for_fill

                    # For a buy order, we capture spread if our fill price is less than the mid-price at entry
                    gross_spread_profit = (order.mid_price_at_entry - order.price) * our_fill
                    net_spread_profit = gross_spread_profit - fee_for_fill
                    self.spread_capture_pnl += net_spread_profit
                    
                    # Track win/loss
                    self.trades_total += 1
                    if net_spread_profit > 0:
                        self.trades_won += 1
                else: # sell order
                    self.position -= our_fill
                    self.cash += order.price * our_fill
                    # Calculate spread capture PnL
                    trade_value = order.price * our_fill
                    fee_for_fill = trade_value * self.MAKER_FEE_RATE
                    self.cash -= fee_for_fill # For sell orders, fee is also a deduction from cash proceeds.
                    self.total_fees_paid += fee_for_fill

                    # For a sell order, we capture spread if our fill price is greater than the mid-price at entry
                    gross_spread_profit = (order.price - order.mid_price_at_entry) * our_fill
                    net_spread_profit = gross_spread_profit - fee_for_fill
                    self.spread_capture_pnl += net_spread_profit
                    
                    # Track win/loss
                    self.trades_total += 1
                    if net_spread_profit > 0:
                        self.trades_won += 1
                
                # Update risk manager with new position and PnL
                current_equity = self.mark_to_market(order.price)  # Use fill price as estimate
                self.risk_manager.update_position_and_pnl(self.position, current_equity)
                
                # Check for any post-trade risk breaches
                if self.risk_manager.emergency_risk_shutdown():
                    print("🚨 EMERGENCY STOP triggered by risk manager!")
                    self.cancel_all_orders(manual_cancel=True)
                    self.status_print_events.add("emergency_stop")
                
                # Include latency info in fill message
                latency_str = ""
                if order.placement_complete_time:
                    latency_str = f" [Fill Latency: {order_to_fill_latency_us/1000:.2f}ms]"
                
                print(f"✅ {order.side.upper()} FILLED: {our_fill:.6f} @ {order.price}{latency_str}")
                print(f"   New Position: {self.position:.6f} | Cash: {self.cash:.2f} | Net Spread PnL: {self.spread_capture_pnl:.2f} | Fees this fill: {fee_for_fill:.4f}")
                self.status_print_events.add("order_filled")
                self._track_fill()

                if order.remaining_qty < 0.0000001:
                    print(f"{order.side.upper()} Order completely filled!")
                    if order.side == "buy":
                        self.open_bid_order = None
                    else:
                        self.open_ask_order = None
                    return True
        else:
            print(f"Queue for {order.side.upper()} order reduced by {trade_qty:.6f}, remaining ahead: {order.current_queue:.6f}")
        return False

    def simulate_fill(self, trade_price, trade_qty, trade_side):
        filled_bid = False
        filled_ask = False

        if self.open_bid_order:
            filled_bid = self._simulate_fill_single_order(self.open_bid_order, trade_price, trade_qty, trade_side)
            if filled_bid and not self.open_bid_order:
                pass
        
        if self.open_ask_order:
            filled_ask = self._simulate_fill_single_order(self.open_ask_order, trade_price, trade_qty, trade_side)
            if filled_ask and not self.open_ask_order:
                pass

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
                # Show key latency metrics
                if 'market_data' in lat_summary:
                    md_lat = lat_summary['market_data']['p95_ms']
                    latency_str += f" | MD:{md_lat:.1f}ms"
                if 'order_placement' in lat_summary:
                    op_lat = lat_summary['order_placement']['p95_ms']
                    latency_str += f" OP:{op_lat:.1f}ms"
                if 'order_to_fill' in lat_summary:
                    of_lat = lat_summary['order_to_fill']['p95_ms']
                    latency_str += f" OF:{of_lat:.1f}ms"
                
                # Add spike warning
                if self.latency_tracker.should_alert():
                    latency_str += " ⚠️LAT"
        
        # Add performance metrics to status (every 10th print to avoid clutter)
        perf_str = ""
        if len(self.pnl_history) > 0 and len(self.pnl_history) % 10 == 0:
            sharpe = self.calculate_sharpe_ratio()
            win_rate = self.get_win_rate()
            dd_pct = self.max_drawdown_observed * 100
            perf_str = f" | Sharpe:{sharpe:.2f} WR:{win_rate:.1f}% DD:{dd_pct:.1f}%"
        
        print(f"Pos: {self.position:.4f} | Cash: {self.cash:.2f} | MTM PnL: {pnl:.2f} | Net Spread PnL: {self.spread_capture_pnl:.2f} | Unrealized: {unrealized_pnl:.2f} | Total Fees: {self.total_fees_paid:.2f}{ot_str}{risk_str}{latency_str}{perf_str} | {orders_info}{events_str}")
        
        # Clear events and update timestamp
        self.status_print_events.clear()
        self.last_status_print_time = now

    def mark_to_market(self, mid_price):
        return self.position * mid_price + self.cash
    
    def mark_to_market_pnl(self, mid_price):
        """Return only the profit/loss component (excluding initial capital)."""
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
        cancel_start_time = datetime.now(timezone.utc)
        now = cancel_start_time
        self.last_cancel_time = now
        if manual_cancel:
            self.last_manual_cancel_time = now

        reason_str = f" ({reason})" if reason else ""
        
        if side == "buy" and self.open_bid_order:
            # Record cancel latency (simulated)
            cancel_complete_time = datetime.now(timezone.utc)
            cancel_latency_us = (cancel_complete_time - cancel_start_time).total_seconds() * 1_000_000
            self.latency_tracker.add_order_cancel_latency(cancel_latency_us)
            
            print(f"Cancelled BUY order @ {self.open_bid_order.price}{' (MANUAL)' if manual_cancel else ' (AUTO)'}{reason_str} [Cancel Latency: {cancel_latency_us/1000:.2f}ms]")
            self.open_bid_order = None
            self.status_print_events.add("order_cancelled")
        elif side == "sell" and self.open_ask_order:
            # Record cancel latency (simulated)
            cancel_complete_time = datetime.now(timezone.utc)
            cancel_latency_us = (cancel_complete_time - cancel_start_time).total_seconds() * 1_000_000
            self.latency_tracker.add_order_cancel_latency(cancel_latency_us)
            
            print(f"Cancelled SELL order @ {self.open_ask_order.price}{' (MANUAL)' if manual_cancel else ' (AUTO)'}{reason_str} [Cancel Latency: {cancel_latency_us/1000:.2f}ms]")
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

    def get_position(self):
        return self.position

    def get_cash(self):
        return self.cash
    
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
            if 'market_data' in latency_summary and latency_summary['market_data']['p95_ms'] > 10:
                latency_penalty += 0.5
            if 'order_placement' in latency_summary and latency_summary['order_placement']['p95_ms'] > 25:
                latency_penalty += 0.5
            if 'order_to_fill' in latency_summary and latency_summary['order_to_fill']['p95_ms'] > 100:
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
    
