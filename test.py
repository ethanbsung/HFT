import os
from dotenv import load_dotenv

load_dotenv()

api_key = os.getenv("COINBASE_API_KEY")
api_secret = os.getenv("COINBASE_API_SECRET")

print("ENV content:", repr(api_key))
print("ENV content:", repr(api_secret))