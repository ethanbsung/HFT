import websockets
import asyncio
import pandas as pd
import json
import os
from datetime import datetime, timezone
from utils.quote_engine import QuoteEngine
import os

class Tradestream:
    def __init__(self, symbol: str = "BTC-USD", quote_engine=None):
        self._symbol = symbol
        # Updated to Coinbase Exchange WebSocket URL
        self._uri = "wss://ws-feed.exchange.coinbase.com"
        self.quote_engine = quote_engine
        os.makedirs("data/trades", exist_ok=True)
    
    async def load_data(self):
        trades = []
        
        async with websockets.connect(self._uri) as ws:
            print(f"Connected to Coinbase Exchange trades WebSocket: {self._uri}")
            
            # Subscribe to matches channel for trade data
            subscribe_message = {
                "type": "subscribe",
                "product_ids": [self._symbol],
                "channels": ["matches"]
            }
            
            await ws.send(json.dumps(subscribe_message))
            print(f"Subscribed to matches trade stream for {self._symbol}")
            
            while True:
                try:
                    message = json.loads(await ws.recv())
                    print(f"📨 TRADE WS: Received message type: {message.get('type')} | Product: {message.get('product_id', 'N/A')}")
                    
                    # Handle subscription confirmation
                    if message.get("type") == "subscriptions":
                        print(f"Trade subscription confirmed for {self._symbol}")
                        print(f"📝 Full trade subscription message: {message}")
                        continue
                    
                    # Handle match (trade) messages
                    elif message.get("type") == "match" and message.get("product_id") == self._symbol:
                        print(f"📊 TRADE: {message.get('side', 'N/A')} {message.get('size', 'N/A')} @ {message.get('price', 'N/A')}")
                        
                        # Convert Coinbase trade format to internal format
                        timestamp_str = message.get("time", "")
                        
                        # Parse timestamp from Coinbase format
                        if timestamp_str:
                            # Remove 'Z' and parse ISO format
                            timestamp = datetime.fromisoformat(timestamp_str.replace('Z', '+00:00'))
                            timestamp_ms = int(timestamp.timestamp() * 1000)
                        else:
                            timestamp_ms = int(datetime.now(timezone.utc).timestamp() * 1000)
                        
                        trade = {
                            "timestamp": timestamp_ms,
                            "price": float(message.get("price", 0)),
                            "quantity": float(message.get("size", 0)),
                            "side": message.get("side", "")  # Coinbase provides side directly (buy/sell)
                        }
                        
                        if self.quote_engine:
                            self.quote_engine.simulate_fill(
                                trade_price=trade["price"],
                                trade_qty=trade["quantity"],
                                trade_side=trade["side"]
                            )
                        print(f"✅ Trade processed: {trade}")
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
                    print(f"🚨 Raw trade message that caused error: {message if 'message' in locals() else 'N/A'}")
                    import traceback
                    traceback.print_exc()
                    await asyncio.sleep(1)
    
    async def main(self):
        await self.load_data()
