import websockets
import asyncio
import pandas as pd
import json
from datetime import datetime, timezone
from utils.quote_engine import QuoteEngine

class Tradestream:
    def __init__(self, symbol: str = "btcusdt", quote_engine=None):
        self._symbol = symbol
        self._uri = f"wss://stream.binance.com:9443/ws/{self._symbol}@trade"
        self.quote_engine = quote_engine
    
    async def load_data(self):
        trades = []
        async with websockets.connect(self._uri) as ws:
            print(f"Connected to {self._uri}")
            while True:
                try:
                    data = json.loads(await ws.recv())
                    trade = {
                        "timestamp": data["T"],
                        "price": float(data["p"]),
                        "quantity": float(data["q"]),
                        "side" : "buy" if not data["m"] else "sell"
                    }
                    if self.quote_engine:
                        self.quote_engine.simulate_fill(
                            trade_price=trade["price"],
                            trade_qty=trade["quantity"],
                            trade_side=trade["side"]
                        )
                    print(f"Trade received: {trade}")
                    trades.append(trade)

                    if len(trades) >= 1000:
                        df = pd.DataFrame(trades)
                        now_str = datetime.now(timezone.utc).strftime("%Y-%m-%d_%H-%M-%S")
                        filename = f"data/trades/{self._symbol}_trades_{now_str}.parquet"
                        df.to_parquet(filename, index=False)
                        print(f"Saved {filename}")
                        trades.clear()
                except Exception as ex:
                    print(f"Error receiving data: {ex}")
                    import traceback
                    traceback.print_exc()
    
    async def main(self):
        await self.load_data()
