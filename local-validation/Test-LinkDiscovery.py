"""Execute production advertisement bodies against the mDNS API boundary."""
from pathlib import Path
import argparse,subprocess,os
p=argparse.ArgumentParser();p.add_argument('--source',type=Path);p.add_argument('--zig',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[1]
source=a.source or root/'firmware/DSPi_ESP32_Front_Panel_v1_1_2/DspiLinkHub.cpp'
text=source.read_text(encoding='utf-8-sig')
start=text.index('void advertise(void*){');stop=text.index('\nbool apply(',start)
body=text[start:stop]
registration=next(line for line in text.splitlines() if 'MDNS.addService(' in line)
out=root/'build/link-discovery';out.mkdir(parents=True,exist_ok=True)
cpp=out/'discovery.cpp';exe=out/'discovery.exe'
fixture=r'''
#include <cassert>
#include <string>
#include <map>
#include <cstdio>
struct String:std::string{String(unsigned n):std::string(std::to_string(n)) {}};
struct Advert{const char* name;const char* hub_id;const char* auth;unsigned devices;const char* serial;};
using hub_advert_t=Advert;static Advert sample{"Panel","hub123","pin",1,"0123456789ABCDEF"};
struct State{bool mdns=true;int hub=0;struct {struct {const char* name="Panel";} bridge;} settings;} storage;
static State* state=&storage;
char* hub_info_json(int*){return nullptr;}void setNetworkInfo(char*){}void cJSON_free(char*){}
void hub_advert(int*,Advert* a){*a=sample;}
struct HostMdns{std::string instance,service,protocol;unsigned port=0,calls=0;std::map<std::string,std::string> txt;
 void setInstanceName(const char* v){instance=v;}
 void addService(const char* s,const char* p,unsigned n){service=s;protocol=p;port=n;}
 template<class T>void addServiceTxt(const char* s,const char* p,const char* k,T v){assert(std::string(s)=="dspi"&&std::string(p)=="tcp");++calls;txt[k]=v;}
}MDNS;
int mdns_service_instance_name_set(const char* s,const char* p,const char* name){assert(std::string(s)=="_dspi"&&std::string(p)=="_tcp");MDNS.instance=name;return 0;}
int mdns_service_txt_item_remove(const char* s,const char* p,const char* key){assert(std::string(s)=="_dspi"&&std::string(p)=="_tcp");MDNS.txt.erase(key);return 0;}
'''
checks=r'''
int main(){
 registerService();assert(MDNS.service=="dspi"&&MDNS.protocol=="tcp"&&MDNS.port==11915);
 assert(MDNS.txt["v"]=="1"&&MDNS.txt["kind"]=="bridge"&&MDNS.txt["auth"]=="pin"&&MDNS.txt["web"]=="1");
 assert(MDNS.txt["hid"]=="hub123"&&MDNS.txt["n"]=="1"&&MDNS.txt["d"]=="0123456789ABCDEF");
 sample.name="Renamed panel";advertise(nullptr);assert(MDNS.instance=="Renamed panel");
 sample.serial=nullptr;sample.devices=0;advertise(nullptr);assert(MDNS.txt.count("d")==0&&MDNS.txt["n"]=="0");
 const auto calls=MDNS.calls;state->mdns=false;advertise(nullptr);assert(MDNS.calls==calls);
 state=nullptr;advertise(nullptr);assert(MDNS.calls==calls);
 puts("PASS: actual Link registration/TXT, hub rename, absent DSPi cleanup and inactive guard");
}
'''
cpp.write_text(fixture+body+'\nvoid registerService(){'+registration+'}\n'+checks)
env=os.environ.copy();env['ZIG_GLOBAL_CACHE_DIR']=str(out/'zig-cache');env['ZIG_LOCAL_CACHE_DIR']=env['ZIG_GLOBAL_CACHE_DIR']
subprocess.run([str(a.zig),'c++','-std=c++17','-O0','-UNDEBUG',str(cpp),'-o',str(exe)],env=env,check=True)
subprocess.run([str(exe)],check=True)
