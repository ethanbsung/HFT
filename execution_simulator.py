class ExecutionSimulator:
    def __init__(self):
        self.live_orders = {}   # order_id -> dict
        self.cash = 0.0
        self.position = 0.0
        self.fills = []        # list of dicts

    def submit_order(self, order: dict):
        """order = {'id': str, 'side': 'buy'|'sell', 'qty': float, 'price': float}"""
        self.live_orders[order['id']] = order
        print(f"📝 EXEC_SIM: Order submitted - {order['side'].upper()} {order['qty']:.6f} @ {order['price']:.2f} [ID: {order['id'][:12]}...]")

    def cancel_order(self, order_id: str):
        cancelled_order = self.live_orders.pop(order_id, None)
        if cancelled_order:
            print(f"❌ EXEC_SIM: Order cancelled - {cancelled_order['side'].upper()} {cancelled_order['qty']:.6f} @ {cancelled_order['price']:.2f} [ID: {order_id[:12]}...]")

    def on_orderbook_update(self, best_bid: float, best_ask: float, ts):
        """Simple FIFO model:  
           • BUY fills if best_ask ≤ order.price  
           • SELL fills if best_bid ≥ order.price  
           • Always fill full qty; no partials yet."""
        to_remove = []
        for oid, o in self.live_orders.items():
            if (o['side'] == 'buy' and best_ask <= o['price']) or \
               (o['side'] == 'sell' and best_bid >= o['price']):
                fill_qty = o['qty']
                px = o['price']
                old_position = self.position
                old_cash = self.cash
                self.position += fill_qty if o['side']=='buy' else -fill_qty
                self.cash     -= px*fill_qty if o['side']=='buy' else -px*fill_qty
                self.fills.append({'oid': oid, 'side': o['side'], 'qty':fill_qty, 'price':px,'ts':ts})
                print(f"✅ EXEC_SIM: FILL! {o['side'].upper()} {fill_qty:.6f} @ {px:.2f} | Pos: {old_position:.6f}→{self.position:.6f} | Cash: {old_cash:.2f}→{self.cash:.2f}")
                to_remove.append(oid)
        for oid in to_remove:  
            self.live_orders.pop(oid, None)

    def mark_to_market(self, mid):
        return self.cash + self.position*mid 