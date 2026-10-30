#!/usr/bin/env python3
"""Run the real chat client on a host with simulated sockets; no devices/secrets.

Requires g++, gcc and ESP-IDF's cJSON source (IDF_PATH or a local ~/esp install).
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    "freertos/FreeRTOS.h": '#pragma once\n#define pdPASS 1\n#define pdTRUE 1\n#define pdMS_TO_TICKS(x) (x)\n',
    "freertos/task.h": '#pragma once\n#include "FreeRTOS.h"\nusing TaskHandle_t=void*;\ninline int xTaskCreate(void(*)(void*),const char*,unsigned,void*,int,TaskHandle_t* t){*t=(void*)1;return pdPASS;}\ninline void xTaskNotifyGive(TaskHandle_t){}\ninline void ulTaskNotifyTake(int,unsigned){}\n',
    "esp_timer.h": '#pragma once\n#include <cstdint>\ninline int64_t test_now=1000000;\ninline int64_t esp_timer_get_time(){return test_now;}\n',
    "settings.h": '#pragma once\n#define SETTINGS_H\n#include <map>\n#include <string>\nclass Settings {public: static inline std::map<std::string,std::string> values; Settings(const std::string&,bool=false){} std::string GetString(const std::string& k){return values[k];} void SetString(const std::string& k,const std::string& v){values[k]=v;}};\n',
    "web_socket.h": r'''#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
class WebSocket {
public:
 static inline std::vector<std::string> sent;
 static inline bool connect_ok=true, send_ok=true;
 static inline std::string hello=R"({"type":"hello","protocol":1,"device_identity":true})";
 static inline std::string url;
 static inline std::map<std::string,std::string> headers;
 bool connected=false;
 std::function<void(const char*,size_t,bool)> data;
 std::function<void()> disconnected;
 ~WebSocket(){if(connected && disconnected)disconnected();}
 void SetHeader(const char* k,const char* v){headers[k]=v;}
 void SetReceiveBufferSize(size_t){}
 bool Connect(const char* u){url=u;connected=connect_ok;if(connected&&!hello.empty())data(hello.data(),hello.size(),false);return connected;}
 bool Send(const std::string& s){if(!send_ok)return false;sent.push_back(s);return true;}
 void OnData(std::function<void(const char*,size_t,bool)> f){data=f;}
 void OnDisconnected(std::function<void()> f){disconnected=f;}
 void OnError(std::function<void(int)>){ }
};
''',
    "board.h": '#pragma once\n#include <memory>\n#include "web_socket.h"\nclass FakeNetwork {public:std::unique_ptr<WebSocket>CreateWebSocket(int id){return std::make_unique<WebSocket>();}};\nclass Board {FakeNetwork network;public:static Board& GetInstance(){static Board b;return b;}FakeNetwork* GetNetwork(){return &network;}};\n',
}
HARNESS = r'''
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#define private public
#include "nanomuse_chat_client.h"
#undef private
#include "web_socket.h"
#include "settings.h"
#include "esp_timer.h"
using State=NanoMuseChatSnapshot::State;
using Error=NanoMuseChatSnapshot::Error;
static auto& client=NanoMuseChatClient::GetInstance();
static void rx(const std::string& json){client.Receive(client.generation_,json.data(),json.size(),false);}
static void event(const char* type,uint32_t id,const char* tail=""){
 rx("{\"type\":\""+std::string(type)+"\",\"id\":"+std::to_string(id)+tail+"}");
}
int main(){
 assert(!client.ConfigureKey("short"));assert(!client.ConfigureKey("abcdefghijklmnop\n"));
 assert(client.Start("http://localhost:8765"));client.Step();assert(!client.Snapshot().ready);
 assert(WebSocket::sent.empty());
 assert(client.ConfigureKey("test-device-key-123456"));
 assert(Settings::values["chat_key"]=="test-device-key-123456");
 assert(client.Start("https://localhost:8765"));client.Step();
 assert(WebSocket::url=="wss://localhost:8765/device");
 assert(WebSocket::headers["Authorization"]=="Bearer test-device-key-123456");
 assert(client.Snapshot().state==State::Ready);
 assert(WebSocket::sent.back().find("metalio_claw_4")!=std::string::npos);
 assert(!client.Send(" \n\t"));assert(!client.Send(std::string(4001,'x')));
 assert(!client.Send(std::string("abc\0def",7)));
 assert(client.Send("你好"));auto first=client.turn_id_;assert(!client.Send("duplicate"));
 client.Step();assert(WebSocket::sent.back().find("你好")!=std::string::npos);
 event("reply",first+1,",\"text\":\"stale\"");assert(client.Snapshot().messages.size()==1);
 event("sent",first);assert(client.Snapshot().state==State::Running);
 auto activity=client.activity_at_;test_now+=1000000;
 event("progress",first,",\"seq\":1");assert(client.activity_at_>activity);
 activity=client.activity_at_;test_now+=1000000;
 event("progress",first,",\"seq\":1");assert(client.activity_at_==activity);
 event("approval",first,",\"text\":\"Review on webpage\"");assert(client.Snapshot().state==State::Approval);
 event("approval_closed",first);assert(client.Snapshot().state==State::Running);
 event("reply",first,",\"text\":\"你好，我是 nanoMuse\"");event("done",first);
 assert(client.Snapshot().state==State::Ready && client.Snapshot().messages.back().text=="你好，我是 nanoMuse");
 assert(client.Send("继续"));auto second=client.turn_id_;assert(second>first);client.Step();
 client.Cancel();event("reply",second,",\"text\":\"late cancelled reply\"");
 assert(client.Snapshot().messages.back().text=="继续");client.Step();
 assert(WebSocket::sent.back().find("cancel")!=std::string::npos);
 assert(client.Send("next"));auto third=client.turn_id_;client.Step();
 event("error",third,",\"text\":\"model unavailable\"");assert(client.Snapshot().state==State::Error);
 assert(client.Send("retry"));client.Step();test_now+=181000000;client.Step();
 assert(client.Snapshot().error==Error::Timeout && !client.Snapshot().Busy());
 assert(WebSocket::sent.back().find("cancel")!=std::string::npos);
 // A dropped request is not replayed by automatic reconnect.
 assert(client.Send("disconnect me"));client.Step();
 auto old_callback=client.socket_->data;auto old_generation=client.generation_;
 client.Disconnected(old_generation);auto send_count=WebSocket::sent.size();
 client.Step();assert(!client.socket_);test_now+=5000001;client.Step();
 assert(client.Snapshot().ready);assert(WebSocket::sent.size()==send_count+1); // identity only
 auto count=client.Snapshot().messages.size();
 const char* stale="{\"type\":\"reply\",\"id\":1,\"text\":\"old socket\"}";
 old_callback(stale,strlen(stale),false);assert(client.Snapshot().messages.size()==count);
 // Wrong protocol, malformed JSON and oversized frames never enable sending.
 rx("{\"type\":\"hello\",\"protocol\":2}");assert(!client.Snapshot().ready);
 assert(client.Start("http://localhost:8765"));client.Step();rx("{");assert(client.Snapshot().error==Error::Protocol);
 assert(client.Start("http://localhost:8765"));client.Step();rx(std::string(8193,'x'));assert(!client.Snapshot().ready);
 // Legacy gateways receive no new metadata, while text still works.
 WebSocket::hello=R"({"type":"hello","protocol":1})";send_count=WebSocket::sent.size();
 assert(client.Start("http://localhost:8765"));client.Step();assert(WebSocket::sent.size()==send_count);
 for(int i=0;i<15;i++){assert(client.Send("bounded history"));auto id=client.turn_id_;client.Step();event("reply",id,",\"text\":\"ok\"");event("done",id);}
 assert(client.Snapshot().messages.size()==12);
 // Exit fences callbacks and clears pending content before socket destruction.
 assert(client.Send("do not replay"));client.Pause();client.Step();assert(!client.socket_);
 assert(client.Snapshot().messages.empty() && !client.Snapshot().ready);
 assert(client.ConfigureKey(""));assert(!client.HasKey());
 puts("Chat client checks passed: protocol, key separation, replies, progress, cancel, stale callbacks, reconnect, deadlines, bounded history and exit.");
}
'''


class ChatClientTests(unittest.TestCase):
    def test_actual_client(self):
        candidates = []
        if os.environ.get("IDF_PATH"):
            candidates.append(Path(os.environ["IDF_PATH"]))
        candidates.extend((Path.home() / "esp").glob("*"))
        cjson = next((p / "components/json/cJSON/cJSON.c" for p in candidates
                      if (p / "components/json/cJSON/cJSON.c").is_file()), None)
        if cjson is None or not shutil.which("g++") or not shutil.which("gcc"):
            self.skipTest("Requires gcc/g++ and ESP-IDF cJSON source; set IDF_PATH")
        with tempfile.TemporaryDirectory(prefix="metalio-chat-test-") as folder:
            out = Path(folder)
            for name, body in STUBS.items():
                file = out / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(body)
            (out / "main.cc").write_text(HARNESS)
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(["gcc", *flags, "-I", str(cjson.parent), "-c", str(cjson),
                            "-o", str(out / "cjson.o")], check=True)
            subprocess.run(["g++", "-std=c++17", "-pthread", *flags, "-I", str(out),
                            "-I", str(ROOT / "main"), "-I", str(cjson.parent),
                            "-include", str(out / "settings.h"), str(out / "main.cc"),
                            str(ROOT / "main/nanomuse_chat_client.cc"), str(out / "cjson.o"),
                            "-o", str(out / "test")], check=True)
            subprocess.run([str(out / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
