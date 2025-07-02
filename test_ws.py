import os
from coinbase.websocket import WSClient

def on_message(msg):
    print("Message:", msg)

def on_open():
    print("Connected")

def on_close():
    print("Disconnected")

# Test with key file
try:
    print("Testing with key_file...")
    client = WSClient(
        key_file="cdp_api_key.json",
        on_message=on_message,
        on_open=on_open,
        on_close=on_close,
        verbose=True
    )
    print("✅ WSClient created successfully with key_file")
    client.close()
except Exception as e:
    print(f"❌ Error with key_file: {e}")

# Test with None values
try:
    print("\nTesting with api_key=None...")
    client = WSClient(
        api_key=None,
        api_secret=None,
        key_file="cdp_api_key.json",
        on_message=on_message,
        on_open=on_open,
        on_close=on_close,
        verbose=True
    )
    print("✅ WSClient created successfully with None values")
    client.close()
except Exception as e:
    print(f"❌ Error with None values: {e}")
