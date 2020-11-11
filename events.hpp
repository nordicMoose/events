// Copyright (c) 2020 Leevi Riitakangas
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef _EVENTS_HPP
#define _EVENTS_HPP

#include <unordered_map>
#include <vector>
#include <functional>
#include <stdint.h>
#include <concepts>
#include <atomic>
#include <mutex>

#ifdef EVENTS_UNSAFE
#define _E_UNSAFE
#endif

#ifdef EVENTS_NO_MEM_PTR
#define _E_NOMEMPTR
#endif


using es_id_t = uint32_t;

struct EObject;

namespace detail{
#ifdef _E_UNSAFE
using _eobj_ptr_t = void*;
#else
using _eobj_ptr_t = EObject*;
#endif
}

template<class T, class... Args>
concept args_callable = 
requires (T t, Args... args){ 
    {t(args...)} -> std::same_as<void>;
};

namespace detail{

enum class ListenerType
{
    Free,
    Member,
    Callable,
    CallablePtr
};

struct _ListenerStateBase
{

    void IncrementRefCount() noexcept{
        ++ref_count;
    }

    void DecrementRefCount() noexcept
    {
        --ref_count;
        if(ref_count == 0)
            delete this;
    }

    ListenerType type;
    uint64_t ptr = -1;
    std::atomic<uint32_t> ref_count = 1;
    std::atomic<_eobj_ptr_t> obj = nullptr;
};

template<class... Args>
struct _ListenerState : public _ListenerStateBase
{
    std::function<void(std::atomic<_eobj_ptr_t>&, Args...)> fun;
};
}

namespace detail{
struct _ListenerBase;
}

struct EObject
{
    #ifndef _E_UNSAFE

    friend class detail::_ListenerBase;
    template<class...> friend class Listener;

    EObject() = default;

    EObject(const EObject&) noexcept{}

    EObject(EObject&& eo)
    {
        listeners = std::move(eo.listeners);

        for(auto l : listeners)
            l->obj = this;
    }

    ~EObject()
    {
        for(auto l : listeners)
        {
            l->obj = nullptr;
            l->DecrementRefCount();
        }
    }

private:

    void RemoveListenerState(detail::_ListenerStateBase* state)
    {
        std::erase_if(listeners,
        [state](const detail::_ListenerStateBase* l)
        {
            return l == state;
        });

        state->DecrementRefCount();
    }

    void AddListener(detail::_ListenerStateBase* state){
        state->IncrementRefCount();
        listeners.push_back(state);
    }

    std::vector<detail::_ListenerStateBase*> listeners;

    #endif
};

struct EventCore;

template<class...>
struct Event;

namespace detail{
struct _ListenerBase
{
    friend struct ::EventCore;

    template<class...> friend struct ::Event;

    _ListenerBase() = default;

    _ListenerBase(_ListenerBase&& l) noexcept
    : _ListenerBase(l)
    {
        l.state = nullptr;

        if(state)
            state->DecrementRefCount();
    }

    _ListenerBase(const _ListenerBase& l ) noexcept
    {
        state = l.state;

        if(state)
            state->IncrementRefCount();
    }

    ~_ListenerBase()
    {
        if(state)
        {
            state->DecrementRefCount();

            #ifndef _E_UNSAFE

            if(state->type == ListenerType::Member &&
               state->obj && 
               state->ref_count == 1)
                state->obj.load()->RemoveListenerState(state);

            #endif
        }
    }

protected:

    _ListenerStateBase* state = nullptr;
};
}

template<class... Args>
struct Listener : public detail::_ListenerBase 
{
    template<class T> 
    #ifndef _E_UNSAFE
    requires std::derived_from<T,EObject>
    #endif
    Listener(T* object,void(T::* lf)(Args...))
    {
        state = new detail::_ListenerState<Args...>();

        static_cast<detail::_ListenerState<Args...>*>(state)->fun = 
        [lf](std::atomic<detail::_eobj_ptr_t>& obj, Args... args)
        {
            (static_cast<T*>(obj.load())->*lf)(args...);
        };

        #ifndef _E_NOMEMPTR

        char tmp[17];
        sprintf(tmp,"%016x",lf);
        state->ptr = strtoull(tmp,0,16);

        #endif

        state->type = detail::ListenerType::Member;
        state->obj = static_cast<detail::_eobj_ptr_t>(object);

        #ifndef _E_UNSAFE

        object->AddListener(state);

        #endif
    }

    Listener(void(* lf)(Args...)) noexcept
    {
        state = new detail::_ListenerState<Args...>();
    
        static_cast<detail::_ListenerState<Args...>*>(state)->fun = 
        [lf](std::atomic<detail::_eobj_ptr_t>& nil, Args... args)
        {
            lf(args...);
        };

        state->type = detail::ListenerType::Free;
        state->ptr = (uint64_t)(lf);
    }
    
    Listener(args_callable<Args...> auto callable) noexcept
    {
        state = new detail::_ListenerState<Args...>();

        static_cast<detail::_ListenerState<Args...>*>(state)->fun = 
        [callable](std::atomic<detail::_eobj_ptr_t>& nil, Args... args)
        {
            callable(args...);
        };

        state->type = detail::ListenerType::Callable;
    }

    Listener(args_callable<Args...> auto* callable) noexcept
    {
        state = new detail::_ListenerState<Args...>();

        static_cast<detail::_ListenerState<Args...>*>(state)->fun = 
        [callable](std::atomic<detail::_eobj_ptr_t>& nil, Args... args)
        {
            (*callable)(args...);
        };

        state->type = detail::ListenerType::CallablePtr;
        state->ptr = (uint64_t)callable;
    }

    Listener(Listener&& l) noexcept
    : detail::_ListenerBase(std::move(l)) {}

    Listener(Listener& l) noexcept
    : detail::_ListenerBase(l){}
};

template<class T, class... Args> requires std::derived_from<T,EObject>
Listener(T,void(T::* lf)(Args...)) -> Listener<Args...>;


template<class T, class... Args> 
concept constructible_to_listener = 
requires (T a) { Listener<Args...>(a);};


struct EventCore final
{
    template<class...> friend struct Event;

private:

    struct Epair
    {
        std::mutex m;
        std::vector<detail::_ListenerStateBase*> v;
    };

    EventCore() = default;

    static es_id_t EventID() noexcept
    {
        constinit static std::atomic<es_id_t> id_counter = 0;
        
        return ++id_counter;
    }

    static void AddListener(es_id_t id, detail::_ListenerBase& listener)
    {
        listener.state->IncrementRefCount();
        
        eventmap_mut.lock();

        auto& [m,listeners] = eventmap[id];

        m.lock();

        listeners.push_back(listener.state);

        m.unlock();

        eventmap_mut.unlock();
    }

    template<class... Args>
    static void RemoveListener(es_id_t id, detail::_ListenerBase& listener)
    {
        eventmap_mut.lock();
        if(eventmap.find(id) != eventmap.end())
        {
            auto& [m,listeners] = eventmap[id];

            m.lock();

            auto it = std::find_if(listeners.begin(),listeners.end(), 
            [&listener](const detail::_ListenerStateBase* l)
            {
                return listener.state == l ||
                    (listener.state->type != detail::ListenerType::Callable && 
                    #ifdef _E_NOMEMPTR
                    listener.state->type != detail::ListenerType::Member && 
                    #endif
                    listener.state->type == l->type && 
                    listener.state->obj == l->obj && 
                    listener.state->ptr == l->ptr);
            });

            if(it != listeners.end()){
                (*it)->DecrementRefCount();
                listeners.erase(it);
            }
            m.unlock();
        }
        eventmap_mut.unlock();
    }

    static void ClearEvent(es_id_t id)
    {
        eventmap_mut.lock();
        auto i = eventmap.find(id);

        if(i != eventmap.end())
        {
            auto& [m,list] = i->second;

            m.lock();

            for(auto l : list)
                l->DecrementRefCount();

            m.unlock();

            eventmap.erase(id);
        }
        eventmap_mut.unlock();
    }

    template<class... Args>
    static void Invoke(es_id_t id, Args... args)
    {
        eventmap_mut.lock();
        bool locked = true;
        if(eventmap.find(id) != eventmap.end())
        {
            RemoveDeadListeners(id);

            auto list = eventmap[id].v;
            auto& m = eventmap[id].m;

            m.lock();

            for(auto l : list)
                l->IncrementRefCount();

            m.unlock();

            eventmap_mut.unlock();
            locked = false;

            for(auto l : list)
                static_cast<detail::_ListenerState<Args...>*>(l)->fun(l->obj, args...);
            
           
            for(auto l : list)
                l->DecrementRefCount();
        }

        if(locked)
            eventmap_mut.unlock();
    }

    static void RemoveDeadListeners(es_id_t id)
    {
        auto& [m,list] = eventmap[id];

        m.lock();

        std::erase_if(list,[](detail::_ListenerStateBase* l)
        {
            return l->type == detail::ListenerType::Member && l->obj == nullptr;
        });

        m.unlock();
    }
    
    static inline std::unordered_map<es_id_t,Epair> eventmap;
    static inline std::mutex eventmap_mut;
};


template<class... Args>
struct Event
{
    Event(const Event&) : event_id(EventCore::EventID()){}

    Event() : event_id(EventCore::EventID()){}

    Event(Event&& e){
        event_id = e.event_id;
        e.event_id = 0;
    }

    ~Event(){
        EventCore::ClearEvent(event_id);
    }

    void operator+=(constructible_to_listener<Args...> auto&& listener){
        operator+=(listener);
    }

    void operator+=(constructible_to_listener<Args...> auto& listener)
    {
        Listener<Args...> l(listener);

        if(l.state == nullptr || 
        (l.state->type == detail::ListenerType::Member 
            && l.state->obj == nullptr)) 
            return;
        
        EventCore::AddListener(event_id,l);
    }

    void operator-=(constructible_to_listener<Args...> auto&& listener){
        operator-=(listener);
    }

    void operator-=(constructible_to_listener<Args...> auto& listener)
    {
        Listener<Args...> l(listener);

        if(l.state == nullptr || 
        (l.state->type == detail::ListenerType::Member 
            && l.state->obj == nullptr)) 
            return;
            
        EventCore::RemoveListener(event_id,l);
    }

    void Clear(){
        EventCore::ClearEvent(event_id);
    }

    void Invoke(Args... args){
        EventCore::Invoke(event_id,args...);
    }

private:

    std::atomic<es_id_t> event_id = 0;
};

#endif