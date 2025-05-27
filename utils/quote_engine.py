from datetime import datetime, timezone
import copy

class Order:
    def __init__(self, side, price, size, queue_ahead, entry_time=None):
        self.side = side
        self.price = price
        self.qty = size
        self.initial_queue = queue_ahead
        self.current_queue = queue_ahead
        self.filled_qty = 0
        self.remaining_qty = size
        self.filled = False
        self.entry_time = entry_time or datetime.now(timezone.utc)
        self.order_id = f"{side}_{price}_{self.entry_time.timestamp()}"

    def fill(self):
        self.filled = True

class QuoteEngine:
    TICK = 0.01
    MAX_TICKS_AWAY = 3
    ORDER_TTL_SEC = 5.0

    def __init__(self, max_position_size=0.05):
        self.position = 0
        self.cash = 10_000.0
        self.initial_cash = self.cash
        self.open_bid_order = None
        self.open_ask_order = None
        self.last_orderbook = None
        self.last_cancel_time = None
        self.max_position_size = max_position_size

    def place_order(self, side, price, size, current_orderbook):
        """
        Place order only if price matches current best bid/ask
        """

        if side == "buy":
            if self.position + size > self.max_position_size:
                print(f"Cannot place {side} order: exceeds max position size {self.max_position_size}. Current position: {self.position}, order size: {size}")
                return False
            if self.open_bid_order:
                self.cancel_order(side="buy")
            
        elif side == "sell":
            if self.position - size < -self.max_position_size:
                print(f"Cannot place {side} order: exceeds max position size {self.max_position_size}. Current position: {self.position}, order size: {size}")
                return False
            if self.open_ask_order:
                self.cancel_order(side="sell")
        else:
            print(f"Invalid side for place_order: {side}")
            return False

        queue_ahead = self._calculate_queue_position(side, price, current_orderbook)
        if queue_ahead is None:
            # print(f"Cannot place {side} order at {price} - not at best price level")
            return False
        
        new_order = Order(side, price, size, queue_ahead)
        if side == "buy":
            self.open_bid_order = new_order
            print(f"Placed BUY order: {size} @ {price}, queue ahead: {queue_ahead:.6f}")
        elif side == "sell":
            self.open_ask_order = new_order
            print(f"Placed SELL order: {size} @ {price}, queue ahead: {queue_ahead:.6f}")
        return True
        
    def _calculate_queue_position(self, side, price, orderbook):
        """
        Only allow orders at best bid/ask, calculate realistic queue position
        Returns None if price is not at the best level for the given side.
        """
        bids = orderbook['bids']
        asks = orderbook['asks']

        if not bids or not asks:
            print("Orderbook is missing bids or asks.")
            return None

        if side == "buy":
            best_bid = float(bids[0][0])
            if not self._same_price_level(price, best_bid):
                print(f"Cannot place BUY order at {price}. Best bid is {best_bid}.")
                return None
            return float(bids[0][1])
        
        elif side == "sell":
            best_ask = float(asks[0][0])
            if not self._same_price_level(price, best_ask):
                print(f"Cannot place SELL order at {price}. Best ask is {best_ask}.")
                return None
            return float(asks[0][1])
        
        return None
    
    def _update_single_order(self, order: Order, current_orderbook):
        """Helper function to update a single order (bid or ask)."""
        if not order or order.filled: # 'filled' might always be False if we cancel/replace
            return

        age = (current_orderbook['timestamp'] - order.entry_time).total_seconds()
        if age > self.ORDER_TTL_SEC:
            print(f"Order {order.side} @ {order.price} expired (TTL) — cancelling.")
            self.cancel_order(side=order.side)
            return # Order is now None, no further processing

        # Re-check if order is still valid after potential TTL cancel
        if not order: return # Should be handled by self.cancel_order setting the attribute to None

        # Check if the specific order (bid or ask) we are updating still exists
        # This is important because cancel_order above might have nulled it.
        if order.side == "buy" and not self.open_bid_order: return
        if order.side == "sell" and not self.open_ask_order: return


        price = order.price # The price of our existing order
        bids = current_orderbook['bids']
        asks = current_orderbook['asks']

        if not bids or not asks:
            # print("Orderbook is missing bids or asks during update.") # Can be noisy
            return

        current_best_bid = float(bids[0][0])
        current_best_ask = float(asks[0][0])

        if order.side == "buy":
            if not self._same_price_level(price, current_best_bid) and price < current_best_bid:
                pass # Holding queue for now if not at best but still "valid"

            if self._same_price_level(price, current_best_bid):
                current_volume_at_level = float(bids[0][1])
                if hasattr(self, 'last_orderbook') and self.last_orderbook and self.last_orderbook['bids']:
                    # Find our price level in the last orderbook
                    old_volume_at_level = 0
                    for ob_price, ob_vol in self.last_orderbook['bids']:
                        if self._same_price_level(price, float(ob_price)):
                            old_volume_at_level = float(ob_vol)
                            break
                    
                    volume_decrease = max(0, old_volume_at_level - current_volume_at_level if self._same_price_level(price, current_best_bid) else 0)
                    # If our price is still best bid, update queue based on best bid volume change.
                    # This part needs to be more robust: what if our price is no longer best_bid but was?
                    # The volume decrease should refer to the specific price level of our order.
                    
                    # Simplified: if we are still at best bid, update queue based on best bid volume change.
                    if self._same_price_level(price, current_best_bid):
                         volume_decrease_at_best = max(0, float(self.last_orderbook['bids'][0][1]) - current_volume_at_level)
                         order.current_queue = max(0, order.current_queue - volume_decrease_at_best)
            
            # If price drifts too far (e.g., market moves against us, or our order is no longer aggressive enough)
            elif abs(price - current_best_bid) > self.MAX_TICKS_AWAY * self.TICK and price < current_best_bid : # Our bid is too low / uncompetitive
                print(f"BUY Order @ {price} auto-cancelled: too far from best bid ({current_best_bid}).")
                self.cancel_order(side="buy")
            elif price > current_best_bid: # Our bid is crossed by the market's best bid (should not happen if we only place at best)
                print(f"BUY Order @ {price} auto-cancelled: crossed by new best bid ({current_best_bid}).")
                self.cancel_order(side="buy")
            
        elif order.side == "sell":
            # If our ask is no longer at or near the best ask, cancel it.
            if not self._same_price_level(price, current_best_ask) and price > current_best_ask:
                pass # Holding queue for now

            if self._same_price_level(price, current_best_ask):
                current_volume_at_level = float(asks[0][1])
                if hasattr(self, 'last_orderbook') and self.last_orderbook and self.last_orderbook['asks']:
                    old_volume_at_level = 0
                    for ob_price, ob_vol in self.last_orderbook['asks']:
                        if self._same_price_level(price, float(ob_price)):
                            old_volume_at_level = float(ob_vol)
                            break

                    if self._same_price_level(price, current_best_ask):
                        volume_decrease_at_best = max(0, float(self.last_orderbook['asks'][0][1]) - current_volume_at_level)
                        order.current_queue = max(0, order.current_queue - volume_decrease_at_best)
            
            elif abs(price - current_best_ask) > self.MAX_TICKS_AWAY * self.TICK and price > current_best_ask: # Our ask is too high / uncompetitive
                print(f"SELL Order @ {price} auto-cancelled: too far from best ask ({current_best_ask}).")
                self.cancel_order(side="sell")
            elif price < current_best_ask: # Our ask is crossed by the market's best ask
                print(f"SELL Order @ {price} auto-cancelled: crossed by new best ask ({current_best_ask}).")
                self.cancel_order(side="sell")
    
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
        if not order or order.filled:
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
        if cq_before_trade_impact <= 0: cq_before_trade_impact = 0

        if order.current_queue <= 0:
            qty_past_our_turn = trade_qty - cq_before_trade_impact
            if qty_past_our_turn < 0: qty_past_our_turn = 0

            our_fill = min(order.remaining_qty, qty_past_our_turn)

            if our_fill > 0.0000001:
                order.filled_qty += our_fill
                order.remaining_qty -= our_fill

                if order.side == "buy":
                    self.position += our_fill
                    self.cash -= order.price * our_fill
                else:
                    self.position -= our_fill
                    self.cash += order.price * our_fill
                
                print(f"✅ {order.side.upper()} FILLED: {our_fill:.6f} @ {order.price}")
                print(f"   New Position: {self.position:.6f} | Cash: {self.cash:.2f}")

                if order.remaining_qty < 0.0000001:
                    print(f"{order.size.upper()} Order completely filled!")
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

    def print_status(self, mid_price):
        pnl = self.mark_to_market_pnl(mid_price)
        active_orders_str = []
        if self.open_bid_order:
            active_orders_str.append(f"BID@{self.open_bid_order.price} (Q:{self.open_bid_order.current_queue:.2f})")
        if self.open_ask_order:
            active_orders_str.append(f"ASK@{self.open_ask_order.price} (Q:{self.open_ask_order.current_queue:.2f})")
        orders_info = " | ".join(active_orders_str) if active_orders_str else "No open orders"
        
        print(f"Pos: {self.position:.4f} | Cash: {self.cash:.2f} | PnL: {pnl:.2f} | {orders_info}")

    def mark_to_market(self, mid_price):
        return self.position * mid_price + self.cash
    
    def mark_to_market_pnl(self, mid_price):
        """Return only the profit/loss component (excluding initial capital)."""
        return self.position * mid_price + self.cash - self.initial_cash

    def cancel_order(self, side: str):
        if side == "buy" and self.open_bid_order:
            print(f"Cancelled BUY order @ {self.open_bid_order.price}")
            self.open_bid_order = None
            self.last_cancel_time = datetime.now(timezone.utc)
        elif side == "sell" and self.open_ask_order:
            print(f"Cancelled SELL order @ {self.open_ask_order.price}")
            self.open_ask_order = None
            self.last_cancel_time = datetime.now(timezone.utc)
        else:
            print(f"No {side} order to cancel")

    def cancel_all_orders(self):
        if self.open_bid_order:
            self.cancel_order(side="buy")
        if self.open_ask_order:
            self.cancel_order(side="sell")

    def get_open_bid_order(self):
        return self.open_bid_order
    
    def get_open_ask_order(self):
        return self.open_ask_order

    def get_position(self):
        return self.position

    def get_cash(self):
        return self.cash
    
