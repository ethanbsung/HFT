import websockets
import asyncio
import pandas as pd
import json
import os
from datetime import datetime, timezone
from utils.quote_engine import QuoteEngine
import os

class Tradestream:
    def __init__(self, symbol: str = "BTC", quote_engine=None):
        self._symbol = symbol
        # Updated to Hyperliquid WebSocket URL
        self._uri = "wss://api.hyperliquid.xyz/ws"
        self.quote_engine = quote_engine
        os.makedirs("data/trades", exist_ok=True)
    
    async def load_data(self):
        trades = []
        async with websockets.connect(self._uri) as ws:
            print(f"Connected to Hyperliquid trades WebSocket: {self._uri}")
            
            # Subscribe to trades for the symbol
            subscription_msg = {
                "method": "subscribe",
                "subscription": {
                    "type": "trades",
                    "coin": self._symbol
                }
            }
            await ws.send(json.dumps(subscription_msg))
            print(f"Subscribed to trades for {self._symbol}")
            
            while True:
                try:
                    message = json.loads(await ws.recv())
                    
                    # Handle subscription response
                    if message.get("channel") == "subscriptionResponse":
                        print(f"Trade subscription confirmed: {message.get('data', {})}")
                        continue
                    
                    # Handle trade updates
                    if message.get("channel") == "trades":
                        trade_data_list = message["data"]
                        
                        # Hyperliquid sends trades as an array
                        if not isinstance(trade_data_list, list):
                            continue
                            
                        for trade_data in trade_data_list:
                            # Convert Hyperliquid trade format to internal format
                            trade = {
                                "timestamp": trade_data["time"],  # Hyperliquid uses milliseconds
                                "price": float(trade_data["px"]),  # px = price
                                "quantity": float(trade_data["sz"]),  # sz = size
                                "side": "buy" if trade_data["side"] == "B" else "sell"  # B = buy, A = ask/sell
                            }
                            
                            if self.quote_engine:
                                self.quote_engine.simulate_fill(
                                    trade_price=trade["price"],
                                    trade_qty=trade["quantity"],
                                    trade_side=trade["side"]
                                )
                            # print(f"Trade received: {trade}")
                            trades.append(trade)

                        if len(trades) >= 1000:
                            df = pd.DataFrame(trades)
                            now_str = datetime.now(timezone.utc).strftime("%Y-%m-%d_%H-%M-%S")
                            filename = f"data/trades/{self._symbol}_trades_{now_str}.parquet"
                            df.to_parquet(filename, index=False)
                            print(f"Saved {filename}")
                            trades.clear()
                            
                except websockets.exceptions.ConnectionClosed:
                    print(f"Trade WebSocket connection closed to {self._uri}. Reconnecting...")
                    await asyncio.sleep(5)
                    break
                except Exception as ex:
                    print(f"Error receiving trade data: {ex}")
                    import traceback
                    traceback.print_exc()
                    await asyncio.sleep(1)
    
    async def main(self):
        await self.load_data()
