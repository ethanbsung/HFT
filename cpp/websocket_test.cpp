// mm_feed_advanced_trade.cpp
//
// Streams level‑2, trade, ticker, and user channels for the given products.
// Auth via Ed25519 JWT (no passphrase). Handles automatic JWT refresh.
//
// Build:
//   g++ mm_feed_advanced_trade.cpp -std=c++17 -O2 \
//       -lboost_system -lssl -lcrypto -lsodium -lpthread
//
// .env  (quotes optional):
//   HFT_API_KEY=organizations/{org_id}/apiKeys/{key_id}
//   HFT_SECRET_KEY=BASE64_RAW_ED25519_KEY   # 32‑ or 64‑byte key
//   # If you only need read‑only data, VIEW scope is enough; for user channel
//   # add TRADE scope when you create the key.
//
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

#include <sodium.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include <nlohmann/json.hpp>

#include <boost/asio/steady_timer.hpp>

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <functional>

using json   = nlohmann::json;
using client = websocketpp::client<websocketpp::config::asio_tls_client>;
using websocketpp::connection_hdl;

/* ────────────── helpers ────────────── */
static inline void trim(std::string& s){
    auto f=[](unsigned char c){return !std::isspace(c);} ;
    s.erase(s.begin(),std::find_if(s.begin(),s.end(),f));
    s.erase(std::find_if(s.rbegin(),s.rend(),f).base(),s.end());
}
static inline void strip_quotes(std::string& s){
    if(s.size()>1&&((s.front()=='"'&&s.back()=='"')||(s.front()=='\''&&s.back()=='\'')))
        s=s.substr(1,s.size()-2);
}
/* load KEY=value from .env upward */
void load_dotenv(){
    namespace fs=std::filesystem;
    for(fs::path p=fs::current_path();;p=p.parent_path()){
        fs::path env=p/".env";
        if(fs::exists(env)){
            std::ifstream in(env);std::string ln;
            while(std::getline(in,ln)){
                if(ln.empty()||ln[0]=='#')continue;
                auto eq=ln.find('=');if(eq==std::string::npos)continue;
                std::string k=ln.substr(0,eq),v=ln.substr(eq+1);
                trim(k);trim(v);strip_quotes(v);
                if(!std::getenv(k.c_str()))setenv(k.c_str(),v.c_str(),0);
            } break;
        }
        if(p==p.root_path())break;
    }
}
/* b64 → bytes */
std::vector<unsigned char> b64_decode(const std::string& b64){
    BIO*b=BIO_new_mem_buf(b64.data(),b64.size());
    BIO*f=BIO_new(BIO_f_base64());BIO_set_flags(f,BIO_FLAGS_BASE64_NO_NL);b=BIO_push(f,b);
    std::vector<unsigned char> out(b64.size());
    int n=BIO_read(b,out.data(),out.size());BIO_free_all(b);
    out.resize(n>0?n:0);return out;
}
/* bytes → base64url */
std::string b64url(const unsigned char*data,size_t len){
    BIO*b64=BIO_new(BIO_f_base64());BIO_set_flags(b64,BIO_FLAGS_BASE64_NO_NL);
    BIO*mem=BIO_new(BIO_s_mem());BIO_push(b64,mem);BIO_write(b64,data,len);BIO_flush(b64);
    BUF_MEM*p;BIO_get_mem_ptr(mem,&p);std::string s(p->data,p->length);BIO_free_all(b64);
    for(char&c:s) if(c=='+')c='-'; else if(c=='/')c='_';
    s.erase(std::find(s.begin(),s.end(),'='),s.end());return s;
}
std::string b64url(const std::string&s){return b64url((const unsigned char*)s.data(),s.size());}
/* 16‑byte random hex nonce */
std::string rand_hex16(){
    unsigned char buf[16];randombytes_buf(buf,sizeof buf);
    static const char*h="0123456789abcdef";std::string out(32,'0');
    for(int i=0;i<16;++i){out[2*i]=h[buf[i]>>4];out[2*i+1]=h[buf[i]&0xf];}
    return out;
}
/* build 120‑sec JWT */
std::string build_jwt(const std::string& kid,const unsigned char sk[crypto_sign_SECRETKEYBYTES]){
    long now=std::time(nullptr);
    json hdr={{"alg","EdDSA"},{"typ","JWT"},{"kid",kid},{"nonce",rand_hex16()}};
    json pay={{"iss","cdp"},{"sub",kid},{"nbf",now},{"exp",now+120}};
    std::string msg=b64url(hdr.dump())+"."+b64url(pay.dump());
    unsigned char sig[crypto_sign_BYTES];
    crypto_sign_detached(sig,nullptr,(const unsigned char*)msg.data(),msg.size(),sk);
    return msg+"."+b64url(sig,sizeof sig);
}

/* ────────────── main ────────────── */
int main(int argc,char*argv[]){
    if(sodium_init()<0){std::cerr<<"libsodium init failed\n";return 1;}
    load_dotenv();
    const char* KEY=getenv("HFT_API_KEY");
    const char* SEC=getenv("HFT_SECRET_KEY");
    if(!KEY||!SEC){std::cerr<<"HFT_API_KEY / HFT_SECRET_KEY not set\n";return 1;}

    /* derive secret‑key bytes */
    auto raw=b64_decode(SEC);
    unsigned char sk[crypto_sign_SECRETKEYBYTES],pk[crypto_sign_PUBLICKEYBYTES];
    if(raw.size()==crypto_sign_SEEDBYTES) crypto_sign_seed_keypair(pk,sk,raw.data());
    else if(raw.size()==crypto_sign_SECRETKEYBYTES) std::copy(raw.begin(),raw.end(),sk);
    else{std::cerr<<"Secret must be 32 or 64‑byte Ed25519 key\n";return 1;}

    /* collect products from CLI */
    std::vector<std::string> products;
    for(int i=1;i<argc;++i) products.emplace_back(argv[i]);
    if(products.empty()) products={"BTC-USD"};

    /* websocket client */
    client c; c.init_asio();
    c.clear_access_channels(websocketpp::log::alevel::all);
    c.clear_error_channels(websocketpp::log::elevel::all);
    c.set_tls_init_handler([](connection_hdl){
        return std::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv12_client);
    });

    /* build initial JWT */
    std::string jwt=build_jwt(KEY,sk);

    /* timer for refresh */
    auto& io = c.get_io_service();
    auto timer = std::make_shared<boost::asio::steady_timer>(io);

    /* declare refresh_jwt function before it's used */
    std::function<void(connection_hdl)> refresh_jwt;

    refresh_jwt = [&](connection_hdl hdl){
        jwt=build_jwt(KEY,sk);
        // re‑authenticate by sending dummy subscribe with refresh token
        json auth={{"type","ping"},{"jwt",jwt}};
        c.send(hdl,auth.dump(),websocketpp::frame::opcode::text);
        timer->expires_after(std::chrono::seconds(110));
        timer->async_wait([&](const boost::system::error_code&ec){
            if(!ec) refresh_jwt(hdl);
        });
    };

    c.set_open_handler([&](connection_hdl hdl){
        /* subscribe helper */
        auto sub=[&](const std::string& ch){
            json msg={{"type","subscribe"},{"channel",ch},{"product_ids",products},{"jwt",jwt}};
            c.send(hdl,msg.dump(),websocketpp::frame::opcode::text);
            std::cout<<">>> "<<msg<<"\n";
        };
        sub("level2");
        sub("market_trades");
        sub("ticker");
        sub("user");            // comment out if key lacks TRADE scope
        /* kick off JWT refresh loop */
        timer->expires_after(std::chrono::seconds(110));
        timer->async_wait([&](const boost::system::error_code&ec){
            if(!ec) refresh_jwt(hdl);
        });
    });

    c.set_message_handler([&](connection_hdl,client::message_ptr m){
        std::cout<<"<<< "<<m->get_payload()<<'\n';
    });

    websocketpp::lib::error_code ec;
    auto con=c.get_connection("wss://advanced-trade-ws.coinbase.com",ec);
    if(ec){std::cerr<<ec.message()<<'\n';return 1;}
    c.connect(con);
    io.run();
}
