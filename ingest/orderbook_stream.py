import pandas as pd
import asyncio
import json
from datetime import datetime, timezone
import os
import time
import threading
from utils.quote_engine import QuoteEngine
from execution_simulator import ExecutionSimulator
from coinbase.websocket import WSClient

def round_to_tick(price: float, tick_size: float) -> float:
    return round(price / tick_size) * tick_size

class Orderbookstream:
    def __init__(self, symbol: str = "BTC-USD", exec_sim=None, api_key: str = None, api_secret: str = None):
        self._symbol = symbol
        self.api_key = api_key
        self.api_secret = api_secret
        self.orderbook_raw = []
        self.orderbook_features = []
        self.batch_size = 1000
        self.file_counter = 0
        self.quote_engine = QuoteEngine(max_position_size=100.0, exec_sim=exec_sim)
        # Local paper‐trading execution simulator - ensure we use the same instance passed in
        self.exec_sim = exec_sim  # Don't create a new one if None - let main.py handle this
        if not self.exec_sim:
            raise ValueError("exec_sim parameter is required - pass the ExecutionSimulator instance from main.py")
        self.desired_order_size = 10.0  # 10 DEXT per quote
        self.target_inventory = 0.0
        self.inventory_tick_skew_per_unit = 5
        self.max_inventory_skew_ticks = 3
        
        # WebSocket client and data handling
        self.ws_client = None
        self.running = False
        self.last_orderbook = None
        
        # Create data directory if it doesn't exist
        os.makedirs("data/orderbooks", exist_ok=True)
        os.makedirs("data/features", exist_ok=True)

    async def stream_data(self):
        """Stream data using official Coinbase Advanced SDK WebSocket client with reconnection."""
        
        if not self.api_key or not self.api_secret:
            print("❌ ERROR: API key and secret required for Advanced Trade WebSocket")
            print("💡 Please provide api_key and api_secret to Orderbookstream constructor")
            return
        
        print(f"🚀 Starting Coinbase Advanced Trade WebSocket for {self._symbol}")
        print(f"🔐 Using official Coinbase SDK for authentication")
        
        # Set up WebSocket message handler
        def on_message(msg):
            try:
                # The official SDK passes the parsed JSON directly
                if isinstance(msg, dict):
                    message = msg
                elif isinstance(msg, str):
                    message = json.loads(msg)
                else:
                    # Handle any other format
                    message = msg
                
                self._handle_websocket_message(message)
                
            except Exception as e:
                print(f"❌ Error processing WebSocket message: {e}")
                print(f"🚨 Raw message: {msg}")
        
        def on_open():
            print(f"✅ WebSocket connection opened for {self._symbol}")
        
        def on_close():
            print(f"❌ WebSocket connection closed for {self._symbol}")
        
        self.running = True
        reconnect_attempts = 0
        max_reconnect_attempts = 10
        
        while self.running and reconnect_attempts < max_reconnect_attempts:
            try:
                print(f"🔄 Connection attempt {reconnect_attempts + 1}/{max_reconnect_attempts}")
                
                # Create the official Coinbase WebSocket client
                self.ws_client = WSClient(
                    api_key=self.api_key,
                    api_secret=self.api_secret,
                    on_message=on_message,
                    on_open=on_open,
                    on_close=on_close,
                    verbose=True  # Enable debug logging
                )
                
                # Open connection
                self.ws_client.open()
                
                # Subscribe to level2 channel for orderbook data
                self.ws_client.level2(product_ids=[self._symbol])
                print(f"📊 Subscribed to level2 orderbook data for {self._symbol}")
                
                # Reset reconnect counter on successful connection
                reconnect_attempts = 0
                
                # Keep connection alive and process messages
                while self.running:
                    try:
                        # Sleep and check for exceptions
                        self.ws_client.sleep_with_exception_check(sleep=1)
                    except Exception as e:
                        print(f"❌ WebSocket error: {e}")
                        print(f"🔄 Will attempt to reconnect in 5 seconds...")
                        break  # Break inner loop to trigger reconnection
                        
            except Exception as e:
                print(f"❌ Failed to start WebSocket: {e}")
                reconnect_attempts += 1
                if reconnect_attempts < max_reconnect_attempts:
                    wait_time = min(5 * reconnect_attempts, 30)  # Exponential backoff, max 30s
                    print(f"🔄 Reconnection attempt {reconnect_attempts}/{max_reconnect_attempts} in {wait_time}s...")
                    await asyncio.sleep(wait_time)
                else:
                    print(f"❌ Max reconnection attempts ({max_reconnect_attempts}) reached. Giving up.")
                    break
            finally:
                if self.ws_client:
                    try:
                        self.ws_client.close()
                    except:
                        pass
                # Wait before reconnect attempt
                if self.running and reconnect_attempts < max_reconnect_attempts:
                    await asyncio.sleep(5)
    
    def _handle_websocket_message(self, message):
        """Handle incoming WebSocket messages from the official SDK"""
        msg_type = message.get('channel', message.get('type', 'unknown'))
        
        print(f"📨 WS Message - Channel: {msg_type} | Product: {message.get('product_id', 'N/A')}")
        
        # Handle different message types
        if msg_type == 'l2_data':
            # Level 2 orderbook data
            events = message.get('events', [])
            for event in events:
                if event.get('type') == 'snapshot':
                    self._handle_orderbook_snapshot(event)
                elif event.get('type') == 'update':
                    self._handle_orderbook_update(event)
                    
        elif msg_type == 'subscriptions':
            print(f"✅ Subscription confirmed: {message}")
            
        elif msg_type == 'error':
            print(f"🚨 WebSocket Error: {message}")
            
        else:
            print(f"🔍 Unknown message type: {msg_type}")
            print(f"📝 Full message: {message}")
    
    def _handle_orderbook_snapshot(self, snapshot):
        """Process initial orderbook snapshot"""
        product_id = snapshot.get('product_id')
        if product_id != self._symbol:
            return
            
        print(f"📊 Received orderbook snapshot for {product_id}")
        
        # Extract bids and asks from the updates
        bids_raw = []
        asks_raw = []
        
        # Separate bids and asks from updates
        for update in snapshot.get('updates', []):
            side = update.get('side', '')
            price = update.get('price_level', '')
            size = update.get('new_quantity', '')
            
            if side == 'bid':
                bids_raw.append([price, size])
            elif side == 'offer':
                asks_raw.append([price, size])
        
        if bids_raw and asks_raw:
            # Start latency tracking for market data processing
            self.quote_engine._start_market_data_processing()
            
            orderbook = {
                'bids': bids_raw,
                'asks': asks_raw,
                'timestamp': datetime.now(timezone.utc)
            }
            
            print(f"📈 Snapshot - Bids: {len(bids_raw)}, Asks: {len(asks_raw)}")
            if bids_raw:
                print(f"📈 Best Bid: {bids_raw[0][0]}")
            if asks_raw:
                print(f"📉 Best Ask: {asks_raw[0][0]}")
            
            # CRITICAL FIX: Use thread-safe queue instead of dangerous event loop creation
            # Schedule processing on the main event loop instead of creating new ones
            try:
                # Get the main event loop running in the main thread
                main_loop = asyncio.get_running_loop()
                # Schedule the coroutine to run on the main loop thread-safely
                asyncio.run_coroutine_threadsafe(
                    self._process_orderbook_update(orderbook), 
                    main_loop
                )
            except RuntimeError:
                # Fallback: If no running loop, process synchronously (safer than new loop)
                print("⚠️ No running event loop found, processing orderbook synchronously")
                # Call synchronous methods instead of async ones
                self._process_orderbook_sync_safe(orderbook)
    
    def _handle_orderbook_update(self, update):
        """Process incremental orderbook updates"""
        product_id = update.get('product_id')
        if product_id != self._symbol:
            return
            
        print(f"📊 Received orderbook update for {product_id}")
        
        # For simplicity, we'll request a new snapshot rather than maintaining state
        # In production, you'd maintain a proper orderbook and apply incremental updates
        pass
    
    def _process_orderbook_sync_safe(self, orderbook):
        """Safe synchronous processing when async context is unavailable"""
        try:
            # Process orderbook synchronously without async calls
            bids_raw = orderbook['bids']
            asks_raw = orderbook['asks']
            timestamp = orderbook['timestamp']
            
            # Update quote engine with orderbook
            self.quote_engine.update_order_with_orderbook(orderbook)
            
            # Update execution simulator
            base_best_bid_price = float(bids_raw[0][0])
            base_best_ask_price = float(asks_raw[0][0])
            self.exec_sim.on_orderbook_update(base_best_bid_price, base_best_ask_price, timestamp)
            self.exec_sim.update_orderbook(orderbook)
            
            print(f"📊 Processed orderbook snapshot synchronously - Spread: {base_best_ask_price - base_best_bid_price:.4f}")
            
        except Exception as e:
            print(f"❌ Error in sync orderbook processing: {e}")
    
    def _process_orderbook_sync(self, orderbook):
        """DEPRECATED: Dangerous synchronous wrapper - use thread-safe scheduling instead"""
        print("⚠️ WARNING: Using deprecated sync wrapper - this can cause deadlocks")
        # Keep for backward compatibility but don't use the dangerous event loop creation
        self._process_orderbook_sync_safe(orderbook)
    
    def stop(self):
        """Stop the WebSocket stream"""
        self.running = False
        if self.ws_client:
            self.ws_client.close()
                



    async def _process_orderbook_update(self, orderbook):
        """Process orderbook update and execute trading logic"""
        bids_raw = orderbook['bids']
        asks_raw = orderbook['asks']
        timestamp = orderbook['timestamp']
        
        tick_size = self.quote_engine.TICK

        self.quote_engine.update_order_with_orderbook(orderbook)

        # Calculate metrics - handle Coinbase REST format safely
        base_best_bid_price = float(bids_raw[0][0])
        base_best_ask_price = float(asks_raw[0][0])
        spread = base_best_ask_price - base_best_bid_price
        # Update simulator with latest top‑of‑book and orderbook data
        self.exec_sim.on_orderbook_update(base_best_bid_price, base_best_ask_price, timestamp)
        self.exec_sim.update_orderbook(orderbook)

        if spread <= tick_size / 2:
            print(f"Warning: Invalid or tight spread ({spread:.8f}). Best Bid: {base_best_bid_price} | Best Ask: {base_best_ask_price}")
            self.quote_engine.cancel_all_orders(manual_cancel=False)
            await asyncio.sleep(0.1)
            return

        # Handle Coinbase REST API format which may have [price, size, num_orders] or [price, size]
        total_bid_volume = sum(float(b[1]) for b in bids_raw if len(b) >= 2)
        total_ask_volume = sum(float(a[1]) for a in asks_raw if len(a) >= 2)
        obi = 0
        if (total_bid_volume + total_ask_volume) > 0:
            obi = (total_bid_volume - total_ask_volume) / (total_bid_volume + total_ask_volume)

        current_signal_state = "QUOTING"
        current_position = self.quote_engine.get_position()
        inventory_deviation = current_position - self.target_inventory

        # Use risk manager's inventory management for position skewing
        bid_skew, ask_skew = self.quote_engine.get_risk_adjusted_skew(base_best_bid_price, base_best_ask_price)

        target_bid_price = round_to_tick(base_best_bid_price + bid_skew, tick_size)
        target_ask_price = round_to_tick(base_best_ask_price + ask_skew, tick_size)

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
                return
                
        if current_signal_state != "HOLD_CROSSED_SKEW":
            # Position-aware asymmetric OBI-based risk management
            current_position = self.quote_engine.get_position()
            inventory_deviation = current_position - self.target_inventory
            skew_ticks = min(
                self.max_inventory_skew_ticks,
                abs(inventory_deviation) * self.inventory_tick_skew_per_unit,
            )
            
            # Asymmetric thresholds based on position
            # Adjusted condition for order size
            if current_position > self.quote_engine.DEFAULT_ORDER_SIZE / 2:  # When long, be more lenient on asks
                extreme_bid_threshold = 0.65   # Less sensitive (was 0.45)
                extreme_ask_threshold = 0.85  # Less sensitive (was 0.80)
                moderate_bid_threshold = 0.35 # Less sensitive (was 0.20)
                moderate_ask_threshold = 0.55 # Less sensitive (was 0.45)
            elif current_position < -self.quote_engine.DEFAULT_ORDER_SIZE / 2:  # When short, be more lenient on bids
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
                if self.quote_engine.place_order("buy", target_bid_price, self.quote_engine.DEFAULT_ORDER_SIZE, orderbook):
                    current_signal_state = "BID_WIDE (MODERATE_OBI)"
                else:
                    current_signal_state = "HOLD_NO_BID (PRICE)"
            else:
                # Normal bid placement
                if self.quote_engine.place_order("buy", target_bid_price, self.quote_engine.DEFAULT_ORDER_SIZE, orderbook):
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
                if self.quote_engine.place_order("sell", target_ask_price, self.quote_engine.DEFAULT_ORDER_SIZE, orderbook):
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
                if self.quote_engine.place_order("sell", target_ask_price, self.quote_engine.DEFAULT_ORDER_SIZE, orderbook):
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
        
        # Complete tick-to-trade latency measurement after order decisions are made
        self.quote_engine._complete_tick_to_trade()
        
        # Complete market data processing latency measurement
        self.quote_engine._complete_market_data_processing()
        
        # Store for next update
        self.last_orderbook = orderbook
        
        # Only print status and detailed info when trading events occur or on interval
        if self.quote_engine.should_print_status():
            print(
                f"[{timestamp.isoformat()}] Inv:{current_position:.3f} Dev:{inventory_deviation:.3f} BidSkew:{bid_skew:.3f} AskSkew:{ask_skew:.3f} "
                f"OBI:{obi:>5.2f} Strat:{current_signal_state:<22} "
                f"Prices (Base B/A): {base_best_bid_price:>8.2f}/{base_best_ask_price:>8.2f} "
                f"(Tgt B/A): {target_bid_price:>8.2f}/{target_ask_price:>8.2f}"
            )
        self.quote_engine.print_status(mid_price=(base_best_bid_price + base_best_ask_price) / 2)

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
