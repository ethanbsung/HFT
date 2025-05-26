import pandas as pd
import asyncio
import json
import websockets
from datetime import datetime
import os
from utils.quote_engine import QuoteEngine, Order

class Orderbookstream:
    def __init__(self, symbol: str = "btcusdt"):
        self._symbol = symbol
        self._uri = "wss://stream.binance.us:9443/ws/btcusdt@depth10@100ms"
        self.orderbook_raw = []
        self.orderbook_features = []
        self.batch_size = 1000
        self.file_counter = 0
        self.quote_engine = QuoteEngine()
        
        # Create data directory if it doesn't exist
        os.makedirs("data/orderbooks", exist_ok=True)
        os.makedirs("data/features", exist_ok=True)

    async def stream_data(self):
        """Stream data from the orderbook stream"""
        signal = "hold"
        bids = []
        asks = []
        async with websockets.connect(self._uri) as ws:
            print(f"Connected to websocket")
            while True:
                try:
                    data = json.loads(await ws.recv())
                    bids = data["bids"]
                    asks = data["asks"]

                    # Calculate metrics
                    best_bid_price = float(bids[0][0])
                    best_ask_price = float(asks[0][0])
                    spread = best_ask_price - best_bid_price

                    total_bid_volume = sum(float(b[1]) for b in bids)
                    total_ask_volume = sum(float(a[1]) for a in asks)
                    obi = (total_bid_volume - total_ask_volume) / (total_bid_volume + total_ask_volume)
                    
                    # Create timestamp
                    timestamp = datetime.now()

                    if obi > 0.3 and not self.quote_engine.get_open_order():
                        signal = "buy"
                        bid_volume_ahead = float(bids[0][1])
                        self.quote_engine.place_order("buy", best_bid_price, 0.01, queue_ahead=bid_volume_ahead)
                    elif obi < -0.3 and not self.quote_engine.get_open_order():
                        signal = "sell"
                        ask_volume_ahead = float(asks[0][1])
                        self.quote_engine.place_order("sell", best_ask_price, 0.01, queue_ahead=ask_volume_ahead)
                    else:
                        signal = "hold"
                        # Only cancel orders if OBI has moved significantly in the opposite direction
                        if self.quote_engine.get_open_order():
                            order = self.quote_engine.get_open_order()
                            if (order.side == "buy" and obi < -0.1) or (order.side == "sell" and obi > 0.1):
                                self.quote_engine.cancel_order()

                    self.orderbook_raw.append({
                        'timestamp': timestamp,
                        'bids': bids,
                        'asks': asks
                    })
                    
                    # Append to orderbook list
                    self.orderbook_features.append({
                        'timestamp': timestamp,
                        'best_bid': best_bid_price,
                        'best_ask': best_ask_price,
                        'spread': spread,
                        'obi': obi,
                        'signal': signal
                    })
                    
                    print(f"OBI: {obi} | Signal: {signal} | Best bid price: {best_bid_price} | Best ask price: {best_ask_price} | Spread: {spread} | Total bid volume: {total_bid_volume} | Total ask volume: {total_ask_volume}")
                    
                    # Save to parquet every 1000 rows
                    if len(self.orderbook_features) >= self.batch_size:
                        await self._save_to_parquet()
                    
                    self.quote_engine.print_status(mid_price=(best_bid_price + best_ask_price) / 2)

                except Exception as e:
                    print(f"Exception Error: {e}")

    async def _save_to_parquet(self):
        """Save the current orderbook data to a parquet file"""
        try:
            df_raw = pd.DataFrame(self.orderbook_raw)
            raw_path = f"data/orderbooks/orderbook_raw_{self._symbol}_{self.file_counter:06d}.parquet"
            df_raw.to_parquet(raw_path, index=False)

            df_features = pd.DataFrame(self.orderbook_features)
            feat_path = f"data/features/orderbook_features_{self._symbol}_{self.file_counter:06d}.parquet"
            df_features.to_parquet(feat_path, index=False)

            print(f"Saved {len(df_raw)} rows to {raw_path}")
            print(f"Saved {len(df_features)} rows to {feat_path}")
            
            # Clear the list and increment counter
            self.orderbook_raw = []
            self.orderbook_features = []
            self.file_counter += 1
            
        except Exception as e:
            print(f"Error saving to parquet: {e}")
