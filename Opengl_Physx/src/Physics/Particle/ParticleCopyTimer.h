#pragma once
#include "OpenGL.h"
#include <array>

// All methods run with the owning PhysX CUDA context current/locked.
// Query readiness only: never synchronize the render thread for a sample.
class ParticleCopyTimer {
public:
    void Begin(HMODULE driver) {
        measuring=false;
        if(!attempted){
            attempted=true;
            create=reinterpret_cast<Create>(GetProcAddress(driver,"cuEventCreate"));
            record=reinterpret_cast<Record>(GetProcAddress(driver,"cuEventRecord"));
            query=reinterpret_cast<Query>(GetProcAddress(driver,"cuEventQuery"));
            destroy=reinterpret_cast<Query>(GetProcAddress(driver,"cuEventDestroy_v2"));
            elapsed=reinterpret_cast<Elapsed>(GetProcAddress(driver,"cuEventElapsedTime"));
            enabled=create&&record&&query&&destroy&&elapsed;
            if(enabled)for(auto& e:events)if(create(&e,0)!=0){enabled=false;break;}
        }
        if(!enabled)return;
        while(pending){
            int status=query(events[read*2+1]);
            if(status==600)break; // CUDA_ERROR_NOT_READY
            float value=0;
            if(status!=0||elapsed(&value,events[read*2],events[read*2+1])!=0){enabled=false;return;}
            ms=hasResult?ms*.9+value*.1:value;hasResult=true;
            read=(read+1)%4;--pending;
        }
        if(pending<4){measuring=record(events[write*2],nullptr)==0;if(!measuring)enabled=false;}
    }
    void End(){
        if(!measuring)return;
        measuring=false;
        if(record(events[write*2+1],nullptr)!=0){enabled=false;return;}
        write=(write+1)%4;++pending;
    }
    void Release(){if(destroy)for(auto& e:events)if(e){destroy(e);e=nullptr;}}
    bool HasResult()const{return enabled&&hasResult;}
    double Milliseconds()const{return ms;}
private:
    using Create=int(WINAPI*)(void**,unsigned);
    using Record=int(WINAPI*)(void*,void*);
    using Query=int(WINAPI*)(void*);
    using Elapsed=int(WINAPI*)(float*,void*,void*);
    Create create=nullptr;Record record=nullptr;Query query=nullptr,destroy=nullptr;Elapsed elapsed=nullptr;
    std::array<void*,8> events{};
    unsigned read=0,write=0,pending=0;
    bool attempted=false,enabled=false,measuring=false,hasResult=false;
    double ms=0;
};
