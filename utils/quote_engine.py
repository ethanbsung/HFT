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

    def __init__(self):
        self.position = 0
        self.cash = 10_000.0
        self.initial_cash = self.cash
        self.open_order = None
        self.last_orderbook = None
        self.last_cancel_time = None

    def place_order(self, side, price, size, current_orderbook):
        """
        Place order only if price matches current best bid/ask
        """

        queue_ahead = self._calculate_queue_position(side, price, current_orderbook)
        if queue_ahead is None:
            print(f"Cannot place {side} order at {price} - not at best price level")
            return False
        
        if self.open_order:
            self.cancel_order()

        self.open_order = Order(side, price, size, queue_ahead)
        print(f"Placed {side} order: {size} @ {price}, queue ahead: {queue_ahead:.6f}")
        return True
    
    def _calculate_queue_position(self, side, price, orderbook):
        """
        Only allow orders at best bid/ask, calculate realistic queue position
        """
        bids = orderbook['bids']
        asks = orderbook['asks']

        if side == "buy":
            best_bid = float(bids[0][0])
            if not self._same_price_level(price, best_bid):
                return None
            return float(bids[0][1])
        
        elif side == "sell":
            best_ask = float(asks[0][0])
            if not self._same_price_level(price, best_ask):
                return None
            return float(asks[0][1])
        
        return None
    
    def update_order_with_orderbook(self, current_orderbook):
        """
        Critical: Update queue position when orderbook changes
        """
        if self.open_order and not self.open_order.filled:
            age = (current_orderbook['timestamp'] - self.open_order.entry_time).total_seconds()
            if age > self.ORDER_TTL_SEC:
                print("Order expired (TTL) — refreshing")
                self.cancel_order()

        if not self.open_order or self.open_order.filled:
            return
        
        side = self.open_order.side
        price = self.open_order.price
        bids = current_orderbook['bids']
        asks = current_orderbook['asks']

        if side == "buy":
            best_bid = float(bids[0][0])
            if abs(price - best_bid) <= self.MAX_TICKS_AWAY * self.TICK:
                current_volume = float(bids[0][1])
                if hasattr(self, 'last_orderbook') and self.last_orderbook:
                    old_volume = float(self.last_orderbook['bids'][0][1])
                    volume_decrease = max(0, old_volume - current_volume)
                    self.open_order.current_queue = max(0, self.open_order.current_queue - volume_decrease)

            else:
                print(f"Order auto-cancelled: price {price} no longer best bid ({best_bid})")
                self.cancel_order()
        
        elif side == "sell":
            best_ask = float(asks[0][0])
            if abs(price - best_ask) <= self.MAX_TICKS_AWAY * self.TICK:
                current_volume = float(asks[0][1])
                if hasattr(self, 'last_orderbook') and self.last_orderbook:
                    old_volume = float(self.last_orderbook['asks'][0][1])
                    volume_decrease = max(0, old_volume - current_volume)
                    self.open_order.current_queue = max(0, self.open_order.current_queue - volume_decrease)
            else:
                print(f"Order auto-cancelled: price {price} no longer best ask ({best_ask})")
                self.cancel_order()
        
        self.last_orderbook = copy.deepcopy(current_orderbook)

    def _same_price_level(self, a: float, b: float, tick=None) -> bool:
        if tick is None:
            tick = self.TICK
        return abs(a - b) < (tick / 2)

    def simulate_fill(self, trade_price, trade_qty, trade_side):
        if not self.open_order or self.open_order.filled:
            return
        
        if not self._same_price_level(trade_price, self.open_order.price):
            return
        
        # For buy orders, we need the trade to be a sell (market sell hitting our bid)
        # For sell orders, we need the trade to be a buy (market buy hitting our ask)
        if self.open_order.side == "buy" and trade_side != "sell":
            return
        if self.open_order.side == "sell" and trade_side != "buy":
            return
        
        print(f"Trade hits our level: {trade_side} {trade_qty} @ {trade_price}")
        print(f"Queue ahead: {self.open_order.current_queue:.6f}")
        
        # Simulate fill if queue ahead has been cleared
        self.open_order.current_queue -= trade_qty
        
        if self.open_order.current_queue <= 0:
            # ✅ Calculate realistic fill quantity
            overfill = abs(self.open_order.current_queue)
            available_for_us = trade_qty - overfill
            our_fill = min(available_for_us, self.open_order.remaining_qty)
            
            if our_fill > 0:
                # ✅ Handle partial fills properly
                self.open_order.filled_qty += our_fill
                self.open_order.remaining_qty -= our_fill
                
                # Update position
                if self.open_order.side == "buy":
                    self.position += our_fill
                    self.cash -= self.open_order.price * our_fill
                else:
                    self.position -= our_fill
                    self.cash += self.open_order.price * our_fill
                print(f"position: {self.position:.6f} | cash: {self.cash:.2f}")
                
                print(f"✅ FILLED: {our_fill:.6f} @ {self.open_order.price}")
                
                if self.open_order.remaining_qty <= 0:
                    print(f"Order completely filled!")
                    self.open_order = None
        else:
            print(f"Queue reduced by {trade_qty:.6f}, remaining: {self.open_order.current_queue:.6f}")

    def print_status(self, mid_price):
        print(f"Position: {self.position} | Cash: {self.cash} | PnL: {self.mark_to_market_pnl(mid_price):.2f}")

    def mark_to_market(self, mid_price):
        return self.position * mid_price + self.cash
    
    def mark_to_market_pnl(self, mid_price):
        """Return only the profit/loss component (excluding initial capital)."""
        return self.position * mid_price + self.cash - self.initial_cash

    def cancel_order(self):
        if self.open_order:
            print(f"Cancelled {self.open_order.side} order @ {self.open_order.price}")
        self.last_cancel_time = datetime.now(timezone.utc)
        self.open_order = None

    def get_open_order(self):
        return self.open_order

    def get_position(self):
        return self.position

    def get_cash(self):
        return self.cash
    
