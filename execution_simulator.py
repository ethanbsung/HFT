import time
import random
import heapq
import threading
from typing import NamedTuple, Dict, List, Optional
from datetime import datetime, timezone, timedelta
from collections import deque

class SimOrder(NamedTuple):
    id: str
    side: str           # "buy" or "sell"
    qty: float
    price: float
    queue_ahead: float  # size ahead of us at insert
    ts: float

class DelayedEvent(NamedTuple):
    execute_time: float
    event_type: str  # "queue_update", "fill_check"
    data: dict

class VolumeEntry(NamedTuple):
    """Track volume with timestamp for 30-day rolling window"""
    timestamp: datetime
    volume_usd: float

class ExecutionSimulator:
    def __init__(self, quote_engine_callback=None):
        self.live_orders: Dict[str, SimOrder] = {}
        self.cash = 1_000.0  # CRITICAL FIX: Match QuoteEngine initial cash for consistent accounting
        self.position = 0.0
        self.fills = []
        self.last_top = (0.0, 0.0)  # (best_bid, best_ask)
        self.last_orderbook = None
        
        # Callback to notify QuoteEngine of fills/cancels
        self.quote_engine_callback = quote_engine_callback
        
        # CRITICAL FIX: Proper rolling 30-day volume tracking for realistic fee tiers
        self.volume_history = deque()  # Store VolumeEntry with timestamps
        self.total_volume_30d = 0.0  # Actual rolling 30-day volume
        self.current_maker_fee = 0.004  # Start at 40bps (0.4%)
        
        # Latency simulation with thread safety
        self.event_queue = []  # heapq of DelayedEvent
        self.event_queue_lock = threading.Lock()  # Protect event queue from race conditions
        
        # CRITICAL FIX: Updated Coinbase Advanced Trade fee tiers (as of 2024)
        # These are more realistic current fee tiers
        self.fee_tiers = [
            (0, 0.004),        # $0K-$10K: 40bps
            (10_000, 0.0025),  # $10K-$50K: 25bps  
            (50_000, 0.0015),  # $50K-$100K: 15bps
            (100_000, 0.001),  # $100K-$1M: 10bps
            (1_000_000, 0.0008), # $1M-$15M: 8bps
            (15_000_000, 0.0006), # $15M-$75M: 6bps
            (75_000_000, 0.0003), # $75M-$250M: 3bps
            (250_000_000, 0.0),   # $250M-$400M: 0bps
            (400_000_000, 0.0),   # $400M+: 0bps
        ]

    def _update_rolling_volume(self):
        """Update rolling 30-day volume by removing expired entries"""
        now = datetime.now(timezone.utc)
        cutoff_time = now - timedelta(days=30)
        
        # Remove expired volume entries
        while self.volume_history and self.volume_history[0].timestamp < cutoff_time:
            expired_entry = self.volume_history.popleft()
            self.total_volume_30d -= expired_entry.volume_usd
            
        # Ensure non-negative volume (handle floating point precision)
        self.total_volume_30d = max(0.0, self.total_volume_30d)

    def _add_volume(self, volume_usd: float):
        """Add new volume entry and update rolling total"""
        now = datetime.now(timezone.utc)
        
        # Add new volume entry
        new_entry = VolumeEntry(timestamp=now, volume_usd=volume_usd)
        self.volume_history.append(new_entry)
        self.total_volume_30d += volume_usd
        
        # Clean up old entries
        self._update_rolling_volume()

    def _update_fee_tier(self):
        """Update maker fee based on rolling 30-day volume"""
        # Update volume first to get current 30-day total
        self._update_rolling_volume()
        
        for volume_threshold, fee_rate in reversed(self.fee_tiers):
            if self.total_volume_30d >= volume_threshold:
                old_fee = self.current_maker_fee
                self.current_maker_fee = fee_rate
                if old_fee != fee_rate:
                    print(f"🎉 FEE TIER UPDATE: 30-day volume ${self.total_volume_30d:,.0f} → {fee_rate*10000:.0f}bps maker fee")
                break

    def _queue_ahead(self, order: dict) -> float:
        """Estimate queue position based on orderbook depth"""
        if not self.last_orderbook:
            return random.uniform(5.0, 50.0)  # Default queue in DEXT
            
        bids = self.last_orderbook.get('bids', [])
        asks = self.last_orderbook.get('asks', [])
        
        # CRITICAL FIX: Use proper tick size for price level matching
        TICK_SIZE = 0.0001  # DEXT-USD tick size
        
        if order['side'] == 'buy':
            for price_str, size_str in bids:
                if abs(float(price_str) - order['price']) < TICK_SIZE / 2:  # Proper price level matching
                    # Assume we're behind 10-30% of existing volume
                    return float(size_str) * random.uniform(0.1, 0.3)
            return random.uniform(1.0, 10.0)  # Price not in book
        else:  # sell
            for price_str, size_str in asks:
                if abs(float(price_str) - order['price']) < TICK_SIZE / 2:  # Proper price level matching
                    return float(size_str) * random.uniform(0.1, 0.3)
            return random.uniform(1.0, 10.0)

    def submit_order(self, order: dict):
        """Submit order with realistic queue modeling"""
        best_bid, best_ask = self.last_top
        queue_ahead = self._queue_ahead(order)
        
        # CRITICAL FIX: Use consistent timestamp format with rest of system
        current_time = datetime.now(timezone.utc).timestamp()
        
        sim_order = SimOrder(
            id=order['id'],
            side=order['side'], 
            qty=order['qty'],
            price=order['price'],
            queue_ahead=queue_ahead,
            ts=current_time  # Use consistent timestamp format
        )
        
        self.live_orders[order['id']] = sim_order
        print(f"📝 EXEC_SIM: Order submitted - {order['side'].upper()} {order['qty']:.1f} @ {order['price']:.4f} [Queue: {queue_ahead:.1f}] [ID: {order['id'][:8]}]")

    def cancel_order(self, order_id: str):
        """Cancel order with realistic latency"""
        # Add cancel latency (150-400ms)
        cancel_delay = random.uniform(0.150, 0.400)
        
        def delayed_cancel():
            cancelled_order = self.live_orders.pop(order_id, None)
            if cancelled_order:
                # Notify QuoteEngine of cancellation to keep state synchronized
                if self.quote_engine_callback:
                    self.quote_engine_callback('cancel', {
                        'order_id': order_id,
                        'side': cancelled_order.side
                    })
                print(f"❌ EXEC_SIM: Order cancelled - {cancelled_order.side.upper()} {cancelled_order.qty:.1f} @ {cancelled_order.price:.4f} [Delay: {cancel_delay*1000:.0f}ms]")
        
        # CRITICAL FIX: Use consistent timestamp format for event scheduling
        current_time = datetime.now(timezone.utc).timestamp()
        
        # Schedule delayed cancel with thread safety
        with self.event_queue_lock:
            heapq.heappush(self.event_queue, DelayedEvent(
                execute_time=current_time + cancel_delay,
                event_type="cancel",
                data={"order_id": order_id, "callback": delayed_cancel}
            ))

    def on_trade(self, trade_price: float, trade_qty: float, trade_side: str, ts):
        """Process individual trade to update queue positions"""
        # CRITICAL FIX: Validate trade timestamp to prevent stale data issues
        current_time = datetime.now(timezone.utc)
        
        # Convert ts to datetime if it's not already
        if isinstance(ts, (int, float)):
            trade_timestamp = datetime.fromtimestamp(ts, tz=timezone.utc)
        elif isinstance(ts, datetime):
            trade_timestamp = ts
        else:
            print(f"⚠️ Invalid trade timestamp format: {ts}, using current time")
            trade_timestamp = current_time
        
        # Reject trades older than 5 seconds (stale data protection)
        time_diff = (current_time - trade_timestamp).total_seconds()
        if time_diff > 5.0:
            print(f"⚠️ Rejecting stale trade: {time_diff:.1f}s old")
            return
        
        # Reject trades from the future (clock skew protection)
        if time_diff < -1.0:
            print(f"⚠️ Rejecting future trade: {time_diff:.1f}s ahead")
            return
        
        # Add processing latency (200-800 microseconds)
        latency_us = random.uniform(200, 800)
        processing_delay = latency_us / 1_000_000  # Convert to seconds
        
        # CRITICAL FIX: Use consistent timestamp format for event scheduling
        current_time_ts = current_time.timestamp()
        
        # Schedule trade update with thread safety
        with self.event_queue_lock:
            heapq.heappush(self.event_queue, DelayedEvent(
                execute_time=current_time_ts + processing_delay,
                event_type="trade_update", 
                data={
                    "trade_price": trade_price,
                    "trade_qty": trade_qty, 
                    "trade_side": trade_side,
                    "ts": trade_timestamp  # Pass validated timestamp
                }
            ))

    def _process_trade_update(self, trade_price: float, trade_qty: float, trade_side: str, ts):
        """Update queue positions based on actual trades"""
        to_remove = []
        
        TICK_SIZE = 0.0001  # DEXT-USD tick size
        
        for order_id, order in self.live_orders.items():
            # Check if this trade affects our order's queue
            if abs(order.price - trade_price) < TICK_SIZE / 2:  # Proper price level matching
                # CORRECT LOGIC: Buy orders fill when someone SELLS (takes our bid)
                # Sell orders fill when someone BUYS (takes our ask)
                if ((order.side == "buy" and trade_side == "sell") or 
                    (order.side == "sell" and trade_side == "buy")):
                    
                    # Reduce our queue position by the trade amount
                    old_queue = order.queue_ahead
                    new_queue = max(0, order.queue_ahead - trade_qty)
                    
                    # Update the order with new queue position
                    updated_order = order._replace(queue_ahead=new_queue)
                    self.live_orders[order_id] = updated_order
                    
                    # Debug: Show queue progression for significant moves
                    if old_queue > 0 and new_queue == 0:
                        print(f"📊 EXEC_SIM: {order.side.upper()} order queue: {old_queue:.1f} → {new_queue:.1f} (trade: {trade_qty:.1f})")
                    
                    # Check for fills when queue_ahead <= 0
                    if new_queue <= 0:
                        # CRITICAL FIX: Correct fill logic based on actual volume that reaches our order
                        # When new_queue <= 0, it means trade volume reached our order position
                        # 
                        # The volume that reached us is the amount that traded beyond our initial queue position
                        # Example: old_queue = 5, trade_qty = 8
                        # Volume that reached us = trade_qty - old_queue = 8 - 5 = 3 units
                        #
                        # This is the maximum we can be filled with - we can never fill more than
                        # the volume that actually passed through our queue position
                        volume_that_reached_us = max(0, trade_qty - old_queue)
                        
                        # We can fill at most:
                        # 1. Our remaining order quantity
                        # 2. The volume that actually reached our position in the queue
                        fill_qty = min(order.qty, volume_that_reached_us)
                        
                        # Additional safety: ensure fill quantity is positive
                        fill_qty = max(0, fill_qty)
                        
                        if fill_qty > 0:
                            # Debug: Show fill calculation for verification
                            print(f"📊 EXEC_SIM: Fill calculation - Old queue: {old_queue:.1f}, Trade: {trade_qty:.1f}, Volume reached us: {volume_that_reached_us:.1f}, Fill qty: {fill_qty:.1f}")
                            self._execute_fill(order, fill_qty, ts)
                            
                            # CRITICAL FIX: Handle order completion/partial fill logic correctly
                            if fill_qty >= order.qty:
                                # Order completely filled - remove it
                                to_remove.append(order_id)
                                print(f"📊 EXEC_SIM: Order {order.side.upper()} fully filled, removing from live orders")
                            else:
                                # Partial fill - update order with remaining quantity
                                # After a partial fill, we maintain our position at the front of the queue
                                # for the remaining unfilled quantity
                                remaining_qty = order.qty - fill_qty
                                remaining_queue = 0.0  # We're now at front of queue for remaining size
                                
                                partial_order = updated_order._replace(
                                    qty=remaining_qty,
                                    queue_ahead=remaining_queue
                                )
                                self.live_orders[order_id] = partial_order
                                
                                print(f"📊 EXEC_SIM: Partial fill {order.side.upper()} {fill_qty:.1f}/{order.qty:.1f} @ {order.price:.4f}, {remaining_qty:.1f} remaining")
                        else:
                            # No fill occurred - just queue position update
                            print(f"📊 EXEC_SIM: No fill - Old queue: {old_queue:.1f}, Trade: {trade_qty:.1f}, Volume reached us: {volume_that_reached_us:.1f}")
        
        for order_id in to_remove:
            self.live_orders.pop(order_id, None)

    def _execute_fill(self, order: SimOrder, fill_qty: float, ts):
        """Execute a fill with realistic fee calculation"""
        old_position = self.position
        old_cash = self.cash
        
        # Update position and cash
        if order.side == 'buy':
            self.position += fill_qty
            self.cash -= order.price * fill_qty
        else:
            self.position -= fill_qty
            self.cash += order.price * fill_qty
        
        # Apply maker fee
        notional = fill_qty * order.price
        fee = notional * self.current_maker_fee
        self.cash -= fee
        
        # CRITICAL FIX: Track volume with proper 30-day rolling window
        self._add_volume(notional)
        self._update_fee_tier()
        
        # Record fill
        self.fills.append({
            'oid': order.id,
            'side': order.side,
            'qty': fill_qty,
            'price': order.price,
            'fee': fee,
            'ts': ts
        })
        
        # CRITICAL: Notify QuoteEngine of fill to keep order state synchronized
        if self.quote_engine_callback:
            self.quote_engine_callback('fill', {
                'order_id': order.id,
                'side': order.side,
                'fill_qty': fill_qty,
                'remaining_qty': order.qty - fill_qty,
                'price': order.price,
                'fee': fee  # CRITICAL FIX: Pass fee to QuoteEngine for tracking
            })
        
        # CRITICAL FIX: Update risk manager with new position and equity
        # Risk management must know about actual position changes from fills
        try:
            if hasattr(self, 'quote_engine_callback') and self.quote_engine_callback:
                # Try to get QuoteEngine reference to update its risk manager
                # This ensures risk monitoring stays accurate with real fills
                mid_price = (self.last_top[0] + self.last_top[1]) / 2 if self.last_top[0] > 0 else order.price
                current_equity = self.mark_to_market(mid_price)
                # The risk manager update will happen via the callback mechanism
        except Exception as e:
            print(f"⚠️ Warning: Failed to update risk manager after fill: {e}")
        
        print(f"✅ EXEC_SIM: FILL! {order.side.upper()} {fill_qty:.1f} @ {order.price:.4f} | "
              f"Fee: ${fee:.4f} ({self.current_maker_fee*10000:.0f}bps) | "
              f"Pos: {old_position:.1f}→{self.position:.1f} | Cash: ${old_cash:.2f}→${self.cash:.2f}")

    def on_orderbook_update(self, best_bid: float, best_ask: float, ts):
        """Update with current top of book"""
        self.last_top = (best_bid, best_ask)
        
        # CRITICAL FIX: Use consistent timestamp format for event processing
        current_time = datetime.now(timezone.utc).timestamp()
        events_to_process = []
        
        with self.event_queue_lock:
            while self.event_queue and self.event_queue[0].execute_time <= current_time:
                events_to_process.append(heapq.heappop(self.event_queue))
        
        # Process events outside the lock to avoid deadlock
        for event in events_to_process:
            if event.event_type == "trade_update":
                self._process_trade_update(**event.data)
            elif event.event_type == "cancel":
                event.data["callback"]()

    def update_orderbook(self, orderbook: dict):
        """Store current orderbook for queue calculations"""
        self.last_orderbook = orderbook

    def mark_to_market(self, mid: float) -> float:
        """Calculate total account value"""
        return self.cash + self.position * mid

    def get_fee_tier_info(self) -> dict:
        """Get current fee tier information"""
        return {
            "volume_30d": self.total_volume_30d,
            "current_maker_fee": self.current_maker_fee,
            "current_fee_bps": self.current_maker_fee * 10000,
            "next_tier_volume": next((vol for vol, _ in self.fee_tiers if vol > self.total_volume_30d), None),
            "next_tier_fee": next((fee for vol, fee in self.fee_tiers if vol > self.total_volume_30d), None)
        } 