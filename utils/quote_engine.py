from datetime import datetime, timezone, timedelta
import copy

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
        
        print(f"AMENDED {order.side.upper()} order: {old_price} → {new_price} (queue retained: {queue_retention:.1%})")
        self.status_print_events.add("order_amended")
        self._track_order_sent("amend")

    def place_order(self, side, price, size, current_orderbook):
        """
        Intelligently place or maintain orders with amend capability
        """
        if not current_orderbook or not current_orderbook.get('bids') or not current_orderbook.get('asks'):
            print("Warning: Orderbook data missing or incomplete in place_order. Cannot place order.")
            return False
        
        bids = current_orderbook['bids']
        asks = current_orderbook['asks']
        if not bids or not asks:
            print("Warning: Bids or asks missing in place_order. Cannot place order.")
            return False

        mid_price_at_entry = (float(bids[0][0]) + float(asks[0][0])) / 2

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
        if side == "buy":
            self.open_bid_order = new_order
            print(f"Placed BUY order: {size} @ {price}, queue ahead: {queue_ahead:.6f}, mid_at_entry: {mid_price_at_entry:.2f}")
            self.status_print_events.add("order_placed")
            self._track_order_sent("new_bid")
        elif side == "sell":
            self.open_ask_order = new_order
            print(f"Placed SELL order: {size} @ {price}, queue ahead: {queue_ahead:.6f}, mid_at_entry: {mid_price_at_entry:.2f}")
            self.status_print_events.add("order_placed")
            self._track_order_sent("new_ask")
        return True
        
    def _calculate_queue_position(self, side, price, orderbook):
        """
        Calculate queue position - handle both existing and new price levels
        """
        bids = orderbook['bids']
        asks = orderbook['asks']

        if not bids or not asks:
            return None

        if side == "buy":
            # Find our price level in the bid stack
            for i, (bid_price, bid_vol) in enumerate(bids):
                if self._same_price_level(price, float(bid_price)):
                    # We're at this price level - assume we join at back of queue
                    queue_vol = float(bid_vol) * 0.85  # Assume we're behind 85% of existing volume
                    return max(0.01, queue_vol)  # Minimum queue of 0.01 (was 0.005)
            
            # Price not found in current orderbook - estimate based on market behavior
            best_bid = float(bids[0][0])
            if price <= best_bid and (best_bid - price) <= self.BASE_MAX_TICKS_AWAY * self.TICK:
                ticks_away = round((best_bid - price) / self.TICK)
                
                if ticks_away == 0:  # Joining at best bid
                    # Estimate queue based on typical best bid volume
                    best_bid_vol = float(bids[0][1])
                    return max(0.02, best_bid_vol * 0.4)  # Behind 40% of best bid volume, min 0.02
                elif ticks_away == 1:  # One tick behind best
                    return 0.01  # Small queue for near-best levels
                else:  # Further away
                    return 0.005  # Very small queue for distant levels
            return None
        
        elif side == "sell":
            # Find our price level in the ask stack
            for i, (ask_price, ask_vol) in enumerate(asks):
                if self._same_price_level(price, float(ask_price)):
                    queue_vol = float(ask_vol) * 0.85  # Assume we're behind 85% of existing volume
                    return max(0.01, queue_vol)  # Minimum queue of 0.01
            
            # Price not found in current orderbook
            best_ask = float(asks[0][0])
            if price >= best_ask and (price - best_ask) <= self.BASE_MAX_TICKS_AWAY * self.TICK:
                ticks_away = round((price - best_ask) / self.TICK)
                
                if ticks_away == 0:  # Joining at best ask
                    best_ask_vol = float(asks[0][1])
                    return max(0.02, best_ask_vol * 0.4)  # Behind 40% of best ask volume, min 0.02
                elif ticks_away == 1:  # One tick above best
                    return 0.01  # Small queue for near-best levels
                else:  # Further away
                    return 0.005  # Very small queue for distant levels (was 0.002)
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
        """Update queue position based on orderbook changes"""
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
                order.current_queue = max(0, order.current_queue - volume_decrease)
            elif current_vol > 0:
                # Price level reappeared or we're tracking it for first time
                order.current_queue = min(order.current_queue, current_vol)
                
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
                order.current_queue = max(0, order.current_queue - volume_decrease)
            elif current_vol > 0:
                order.current_queue = min(order.current_queue, current_vol)

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

        order.current_queue -= trade_qty

        cq_before_trade_impact = order.current_queue + trade_qty
        if cq_before_trade_impact <= 0: 
            cq_before_trade_impact = 0

        if order.current_queue <= 0:
            qty_past_our_turn = trade_qty - cq_before_trade_impact
            if qty_past_our_turn < 0: 
                qty_past_our_turn = 0

            our_fill = min(order.remaining_qty, qty_past_our_turn)

            if our_fill > 0.0000001:
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
                
                print(f"✅ {order.side.upper()} FILLED: {our_fill:.6f} @ {order.price}")
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
        
        # Add performance metrics to status (every 10th print to avoid clutter)
        perf_str = ""
        if len(self.pnl_history) > 0 and len(self.pnl_history) % 10 == 0:
            sharpe = self.calculate_sharpe_ratio()
            win_rate = self.get_win_rate()
            dd_pct = self.max_drawdown_observed * 100
            perf_str = f" | Sharpe:{sharpe:.2f} WR:{win_rate:.1f}% DD:{dd_pct:.1f}%"
        
        print(f"Pos: {self.position:.4f} | Cash: {self.cash:.2f} | MTM PnL: {pnl:.2f} | Net Spread PnL: {self.spread_capture_pnl:.2f} | Unrealized: {unrealized_pnl:.2f} | Total Fees: {self.total_fees_paid:.2f}{ot_str}{perf_str} | {orders_info}{events_str}")
        
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
        now = datetime.now(timezone.utc)
        self.last_cancel_time = now
        if manual_cancel:
            self.last_manual_cancel_time = now

        reason_str = f" ({reason})" if reason else ""
        
        if side == "buy" and self.open_bid_order:
            print(f"Cancelled BUY order @ {self.open_bid_order.price}{' (MANUAL)' if manual_cancel else ' (AUTO)'}{reason_str}")
            self.open_bid_order = None
            self.status_print_events.add("order_cancelled")
        elif side == "sell" and self.open_ask_order:
            print(f"Cancelled SELL order @ {self.open_ask_order.price}{' (MANUAL)' if manual_cancel else ' (AUTO)'}{reason_str}")
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
        
        # Determine performance grade based on realistic simulation benchmarks
        performance_grade = "Poor"
        if sharpe >= 0.8 and win_rate >= 52 and self.max_drawdown_observed <= 0.03 and ot_ratio <= 15:
            performance_grade = "Excellent"
        elif sharpe >= 0.5 and win_rate >= 50 and self.max_drawdown_observed <= 0.05 and ot_ratio <= 25:
            performance_grade = "Good"
        elif sharpe >= 0.2 and win_rate >= 48 and self.max_drawdown_observed <= 0.08:
            performance_grade = "Acceptable"
        
        return {
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
            'pnl_samples': len(self.pnl_history)
        }
    
