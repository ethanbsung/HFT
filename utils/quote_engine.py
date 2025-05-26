from datetime import datetime, timezone

class Order:
    def __init__(self, side, price, size, queue_ahead):
        self.side = side
        self.price = price
        self.qty = size
        self.queue = queue_ahead
        self.filled = False
        self.entry_time = datetime.now(timezone.utc)

    def fill(self):
        self.filled = True

class QuoteEngine:
    TICK = 0.01

    def __init__(self):
        self.position = 0
        self.cash = 0.0
        self.open_order = None

    def place_order(self, side, price, size, queue_ahead):
        self.open_order = Order(side, price, size, queue_ahead)
        print(f"Placed {side} order: {size} @ {price}, queue ahead: {queue_ahead}")

    def _same_price_level(self, a: float, b: float, tick=None) -> bool:
        if tick is None:
            tick = self.TICK
        return abs(a - b) < tick / 2

    def simulate_fill(self, trade_price, trade_qty, trade_side):
        if not self.open_order or self.open_order.filled:
            return
        
        print(f"Trade: {trade_side} {trade_qty} @ {trade_price}, Order: {self.open_order.side} @ {self.open_order.price}, Queue: {self.open_order.queue}")
        
        if not self._same_price_level(trade_price, self.open_order.price):
            return
        
        # For buy orders, we need the trade to be a sell (market sell hitting our bid)
        # For sell orders, we need the trade to be a buy (market buy hitting our ask)
        if self.open_order.side == "buy" and trade_side != "sell":
            return
        if self.open_order.side == "sell" and trade_side != "buy":
            return
        
        # Simulate fill if queue ahead has been cleared
        self.open_order.queue -= trade_qty
        print(f"Queue reduced by {trade_qty}, remaining queue: {self.open_order.queue}")
        
        if self.open_order.queue <= 0:
            self.open_order.fill()
            print(f"Order filled! {self.open_order.side} {self.open_order.qty} @ {self.open_order.price}")
            if self.open_order.side == "buy":
                self.position += self.open_order.qty
                self.cash -= self.open_order.price * self.open_order.qty
            elif self.open_order.side == "sell":
                self.position -= self.open_order.qty
                self.cash += self.open_order.price * self.open_order.qty
            print(f"New position: {self.position}, Cash: {self.cash:.2f}")
            self.open_order = None  # Clear the filled order

    def print_status(self, mid_price):
        print(f"Position: {self.position} | Cash: {self.cash} | PnL: {self.mark_to_market(mid_price):.2f}")

    def mark_to_market(self, mid_price):
        return self.position * mid_price + self.cash

    def cancel_order(self):
        if self.open_order:
            print(f"Cancelled {self.open_order.side} order @ {self.open_order.price}")
        self.open_order = None

    def get_open_order(self):
        return self.open_order

    def get_position(self):
        return self.position

    def get_cash(self):
        return self.cash
    
