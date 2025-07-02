import requests
import time

def get_products():
    r = requests.get("https://api.exchange.coinbase.com/products")
    return r.json()

def get_orderbook(product_id):
    r = requests.get(f"https://api.exchange.coinbase.com/products/{product_id}/book?level=2")
    return r.json()

def get_stats(product_id):
    r = requests.get(f"https://api.exchange.coinbase.com/products/{product_id}/stats")
    return r.json()

def main():
    products = get_products()
    print(f"Scanning {len(products)} products...")

    for p in products:
        product_id = p['id']
        if not product_id.endswith("-USD"):
            continue  # only scan USD pairs

        try:
            orderbook = get_orderbook(product_id)
            stats = get_stats(product_id)

            # Get best bid and ask from Level 2 order book
            bids = orderbook.get('bids', [])
            asks = orderbook.get('asks', [])
            
            if not bids or not asks:
                continue
                
            # Best bid is highest price (first in sorted bids)
            best_bid = float(bids[0][0])
            # Best ask is lowest price (first in sorted asks)  
            best_ask = float(asks[0][0])
            
            if best_bid <= 0 or best_ask <= 0:
                continue
                
            # Compute spread from actual order book
            spread = (best_ask - best_bid) / ((best_ask + best_bid) / 2)
            volume = float(stats['volume']) * float(stats['last'])  # in USD
            
        except (KeyError, ValueError, IndexError):
            continue

        # realistic MM filter
        if 500_000 <= volume <= 5_000_000 and spread > 0.01:  # i.e. 1%
            print(f"{product_id}: spread={spread*100:.2f}%, vol=${volume/1e6:.2f}M, bid=${best_bid:.4f}, ask=${best_ask:.4f}")

if __name__ == "__main__":
    while True:
        main()
        time.sleep(60)  # re-scan every minute