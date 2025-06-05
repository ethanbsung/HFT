import pandas as pd
import asyncio
import json
import websockets
from datetime import datetime, timezone
import os
import traceback
from utils.quote_engine import QuoteEngine

def round_to_tick(price: float, tick_size: float) -> float:
    return round(price / tick_size) * tick_size

class Orderbookstream:
    def __init__(self, symbol: str = "BTC"):
        self._symbol = symbol
        # Updated to Hyperliquid WebSocket URL
        self._uri = "wss://api.hyperliquid.xyz/ws"
        self.orderbook_raw = []
        self.orderbook_features = []
        self.batch_size = 1000
        self.file_counter = 0
        self.quote_engine = QuoteEngine(max_position_size=2.0)
        self.desired_order_size = 0.1
        self.target_inventory = 0.0
        self.inventory_tick_skew_per_unit = 5
        self.max_inventory_skew_ticks = 3
        
        # Create data directory if it doesn't exist
        os.makedirs("data/orderbooks", exist_ok=True)
        os.makedirs("data/features", exist_ok=True)

    async def stream_data(self):
        """Stream data from the Hyperliquid orderbook stream and manage two-sided quotes."""
        async with websockets.connect(self._uri) as ws:
            print(f"Connected to Hyperliquid WebSocket: {self._uri}")
            
            # Subscribe to l2Book for the symbol
            subscription_msg = {
                "method": "subscribe",
                "subscription": {
                    "type": "l2Book",
                    "coin": self._symbol
                }
            }
            await ws.send(json.dumps(subscription_msg))
            print(f"Subscribed to l2Book for {self._symbol}")
            
            while True:
                try:
                    message = json.loads(await ws.recv())
                    
                    # Handle subscription response
                    if message.get("channel") == "subscriptionResponse":
                        print(f"Subscription confirmed: {message.get('data', {})}")
                        continue
                    
                    # Handle l2Book updates
                    if message.get("channel") == "l2Book":
                        data = message["data"]
                        
                        # Extract orderbook data from Hyperliquid format
                        if not data.get("levels") or len(data["levels"]) != 2:
                            print("Warning: Invalid levels in received data. Skipping update.")
                            await asyncio.sleep(0.1)
                            continue
                        
                        # Convert Hyperliquid format to Binance-compatible format
                        bids_raw = [[level["px"], level["sz"]] for level in data["levels"][0]]
                        asks_raw = [[level["px"], level["sz"]] for level in data["levels"][1]]
                        
                        # Convert Hyperliquid timestamp (milliseconds) to datetime
                        timestamp = datetime.fromtimestamp(data["time"] / 1000, tz=timezone.utc)

                        if not bids_raw or not asks_raw:
                            print("Warning: Empty bids or asks in received data. Skipping update.")
                            await asyncio.sleep(0.1)
                            continue

                        orderbook = {'bids': bids_raw, 'asks': asks_raw, 'timestamp': timestamp}
                        tick_size = self.quote_engine.TICK

                        self.quote_engine.update_order_with_orderbook(orderbook)

                        # Calculate metrics
                        base_best_bid_price = float(bids_raw[0][0])
                        base_best_ask_price = float(asks_raw[0][0])
                        spread = base_best_ask_price - base_best_bid_price

                        if spread <= tick_size / 2:
                            print(f"Warning: Invalid or tight spread ({spread:.8f}). Best Bid: {base_best_bid_price} | Best Ask: {base_best_ask_price}")
                            self.quote_engine.cancel_all_orders(manual_cancel=False)
                            await asyncio.sleep(0.1)
                            continue

                        total_bid_volume = sum(float(b[1]) for b in bids_raw)
                        total_ask_volume = sum(float(a[1]) for a in asks_raw)
                        obi = 0
                        if (total_bid_volume + total_ask_volume) > 0:
                            obi = (total_bid_volume - total_ask_volume) / (total_bid_volume + total_ask_volume)

                        current_signal_state = "QUOTING"
                        current_position = self.quote_engine.get_position()
                        inventory_deviation = current_position - self.target_inventory

                        skew_ticks = inventory_deviation * self.inventory_tick_skew_per_unit

                        if skew_ticks > self.max_inventory_skew_ticks:
                            skew_ticks = self.max_inventory_skew_ticks
                        elif skew_ticks < -self.max_inventory_skew_ticks:
                            skew_ticks = -self.max_inventory_skew_ticks

                        target_bid_price = round_to_tick(base_best_bid_price - (skew_ticks * tick_size), tick_size)
                        target_ask_price = round_to_tick(base_best_ask_price - (skew_ticks * tick_size), tick_size)

                        if target_bid_price >= target_ask_price:
                            print(f"Warning: Skewed bid ({target_bid_price}) is >= skewed ask ({target_ask_price}). Using BBO.")
                            target_bid_price = round_to_tick(base_best_bid_price, tick_size)
                            target_ask_price = round_to_tick(base_best_ask_price, tick_size)
                            if target_bid_price >= target_ask_price:
                                current_signal_state = "HOLD_CROSSED_SKEW"

                        if hasattr(self.quote_engine, 'last_manual_cancel_time') and self.quote_engine.last_manual_cancel_time is not None:
                            if (timestamp - self.quote_engine.last_manual_cancel_time).total_seconds() < 0.3:
                                print(f"Holding quotes due to recent MANUAL cancellation. OBI: {obi:>6.3f}, InvDev: {inventory_deviation:.4f}")
                                current_signal_state = "HOLD_COOLDOWN_MANUAL"

                                self.orderbook_raw.append({
                                    'timestamp': orderbook['timestamp'],
                                    'bids': bids_raw,
                                    'asks': asks_raw
                                })
                                self.orderbook_features.append({
                                    'timestamp': orderbook['timestamp'],
                                    'best_bid': base_best_bid_price,
                                    'best_ask': base_best_ask_price,
                                    'spread': spread,
                                    'obi': obi,
                                    'signal': current_signal_state,
                                    'skewed_bid': target_bid_price,
                                    'skewed_ask': target_ask_price,
                                    'inventory': current_position
                                })
                                self.quote_engine.print_status(mid_price=(base_best_bid_price + base_best_ask_price) / 2, force=True)
                                await asyncio.sleep(0.1)
                                continue
                                
                        if current_signal_state != "HOLD_CROSSED_SKEW":
                            # Position-aware asymmetric OBI-based risk management
                            current_position = self.quote_engine.get_position()
                            
                            # Asymmetric thresholds based on position
                            # Adjusted condition for larger desired_order_size
                            if current_position > self.desired_order_size / 2:  # When long, be more lenient on asks
                                extreme_bid_threshold = 0.65   # Less sensitive (was 0.45)
                                extreme_ask_threshold = 0.85  # Less sensitive (was 0.80)
                                moderate_bid_threshold = 0.35 # Less sensitive (was 0.20)
                                moderate_ask_threshold = 0.55 # Less sensitive (was 0.45)
                            elif current_position < -self.desired_order_size / 2:  # When short, be more lenient on bids
                                extreme_bid_threshold = 0.85  # Less sensitive (was 0.80)
                                extreme_ask_threshold = 0.65   # Less sensitive (was 0.45)
                                moderate_bid_threshold = 0.55 # Less sensitive (was 0.45)
                                moderate_ask_threshold = 0.35 # Less sensitive (was 0.20)
                            else:  # When flat, use balanced thresholds
                                extreme_bid_threshold = 0.70 # Less sensitive (was 0.55)
                                extreme_ask_threshold = 0.70 # Less sensitive (was 0.55)
                                moderate_bid_threshold = 0.40 # Less sensitive (was 0.25)
                                moderate_ask_threshold = 0.40 # Less sensitive (was 0.25)

                            # BID SIDE LOGIC
                            if obi < -extreme_bid_threshold:  # Extreme selling pressure
                                if self.quote_engine.get_open_bid_order():
                                    print(f"OBI ({obi:.2f}) EXTREME for BID (pos: {current_position:.3f}). Cancelling existing bid.")
                                    self.quote_engine.cancel_order("buy", manual_cancel=True)
                                current_signal_state = "HOLD_NO_BID (EXTREME_OBI)"
                            elif obi < -moderate_bid_threshold:  # Moderate selling pressure  
                                # Don't cancel, but widen bid spread
                                target_bid_price = round_to_tick(base_best_bid_price - (skew_ticks * tick_size) - tick_size, tick_size)
                                if self.quote_engine.place_order("buy", target_bid_price, self.desired_order_size, orderbook):
                                    current_signal_state = "BID_WIDE (MODERATE_OBI)"
                                else:
                                    current_signal_state = "HOLD_NO_BID (PRICE)"
                            else:
                                # Normal bid placement
                                if self.quote_engine.place_order("buy", target_bid_price, self.desired_order_size, orderbook):
                                    pass  # Order placed or maintained successfully
                                else:
                                    current_signal_state = "HOLD_NO_BID (PRICE)"

                            # ASK SIDE LOGIC
                            if obi > extreme_ask_threshold:   # Extreme buying pressure
                                if self.quote_engine.get_open_ask_order():
                                    print(f"OBI ({obi:.2f}) EXTREME for ASK (pos: {current_position:.3f}). Cancelling existing ask.")
                                    self.quote_engine.cancel_order("sell", manual_cancel=True)
                                if current_signal_state == "HOLD_NO_BID (EXTREME_OBI)":
                                    current_signal_state = "HOLD_BOTH (EXTREME_OBI)"
                                else:
                                    current_signal_state = "HOLD_NO_ASK (EXTREME_OBI)"
                            elif obi > moderate_ask_threshold:  # Moderate buying pressure
                                # Don't cancel, but widen ask spread  
                                target_ask_price = round_to_tick(base_best_ask_price - (skew_ticks * tick_size) + tick_size, tick_size)
                                if self.quote_engine.place_order("sell", target_ask_price, self.desired_order_size, orderbook):
                                    if current_signal_state == "BID_WIDE (MODERATE_OBI)":
                                        current_signal_state = "BOTH_WIDE (MODERATE_OBI)"
                                    else:
                                        current_signal_state = "ASK_WIDE (MODERATE_OBI)"
                                else:
                                    if current_signal_state == "HOLD_NO_BID (PRICE)":
                                        current_signal_state = "HOLD_BOTH (PRICE)"
                                    else:
                                        current_signal_state = "HOLD_NO_ASK (PRICE)"
                            else:
                                # Normal ask placement
                                if self.quote_engine.place_order("sell", target_ask_price, self.desired_order_size, orderbook):
                                    pass  # Order placed or maintained successfully
                                else:
                                    if current_signal_state == "HOLD_NO_BID (PRICE)":
                                        current_signal_state = "HOLD_BOTH (PRICE)"
                                    else:
                                        current_signal_state = "HOLD_NO_ASK (PRICE)"

                        open_bid = self.quote_engine.get_open_bid_order()
                        open_ask = self.quote_engine.get_open_ask_order()

                        if current_signal_state == "QUOTING":
                            if not open_bid and not open_ask:
                                current_signal_state = "IDLE_NO_ORDERS"
                            elif not open_bid:
                                current_signal_state = "ASK_ONLY"
                            elif not open_ask:
                                current_signal_state = "BID_ONLY"

                        self.orderbook_raw.append({
                            'timestamp': orderbook['timestamp'],
                            'bids': bids_raw,
                            'asks': asks_raw
                        })
                        
                        # Append to orderbook list
                        self.orderbook_features.append({
                            'timestamp': orderbook['timestamp'],
                            'best_bid': base_best_bid_price,
                            'best_ask': base_best_ask_price,
                            'spread': spread,
                            'obi': obi,
                            'signal': current_signal_state,
                            'skewed_bid': target_bid_price,
                            'skewed_ask': target_ask_price,
                            'inventory': current_position
                        })
                        
                        # Save to parquet every 1000 rows
                        if len(self.orderbook_features) >= self.batch_size:
                            await self._save_to_parquet()
                        
                        # Only print status and detailed info when trading events occur or on interval
                        if self.quote_engine.should_print_status():
                            print(
                                f"[{timestamp.isoformat()}] Inv:{current_position:.3f} Dev:{inventory_deviation:.3f} SkewTks:{skew_ticks:.1f} "
                                f"OBI:{obi:>5.2f} Strat:{current_signal_state:<22} "
                                f"Prices (Base B/A): {base_best_bid_price:>8.2f}/{base_best_ask_price:>8.2f} "
                                f"(Tgt B/A): {target_bid_price:>8.2f}/{target_ask_price:>8.2f}"
                            )
                        self.quote_engine.print_status(mid_price=(base_best_bid_price + base_best_ask_price) / 2)

                except websockets.exceptions.ConnectionClosed:
                    print(f"WebSocket connection closed to {self._uri}. Reconnecting...")
                    await asyncio.sleep(5)
                    break
                except Exception as e:
                    print(f"Exception Error in orderbook_stream: {e}")
                    traceback.print_exc()
                    await asyncio.sleep(1)

    async def _save_to_parquet(self):
        try:
            df_raw = pd.DataFrame(self.orderbook_raw)
            raw_path = f"data/orderbooks/orderbook_raw_{self._symbol}_{self.file_counter:06d}.parquet"
            df_raw.to_parquet(raw_path, index=False)

            df_features = pd.DataFrame(self.orderbook_features)
            feat_path = f"data/features/orderbook_features_{self._symbol}_{self.file_counter:06d}.parquet"
            df_features.to_parquet(feat_path, index=False)

            print(f"Saved {len(df_raw)} rows to {raw_path} and {len(df_features)} rows to {feat_path}")
            
            self.orderbook_raw = []
            self.orderbook_features = []
            self.file_counter += 1
            
        except Exception as e:
            print(f"Error saving to parquet: {e}")
