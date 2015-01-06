#pragma once

#include <atomic>
#include <deque>
#include <boost\serialization\version.hpp>
#include <boost\asio\io_service.hpp>
#include <boost\asio\deadline_timer.hpp>

namespace collections {

    class object_registry;

    // The purpose of autorelease_queue is to increase object's lifetime, delay object's release
    class autorelease_queue : boost::noncopyable {
    public:
        typedef std::lock_guard<bshared_mutex> lock;
        typedef uint32_t time_point;

        struct object_lifetime_policy {
            static void retain(object_base * p) {
                p->retain();
            }

            static void release(object_base * p) {
                p->_final_release();
            }
        };

        typedef boost::intrusive_ptr_jc<object_base, object_lifetime_policy> queue_object_ref;
        typedef std::vector<std::pair<Handle, time_point> > queue_old;
        typedef std::deque<std::pair<queue_object_ref, time_point> > queue;

    private:

        object_registry& _registry;
        queue _queue;
        time_point _tickCounter;

        spinlock _queue_mutex;
        spinlock _timer_mutex;
        
        // it's not overkill to use asio?
        boost::asio::io_service _io;
        boost::asio::io_service::work _work;
        boost::asio::deadline_timer _timer;
        std::thread _thread;

        // reusable array for temp objects
        std::vector<queue_object_ref> _toRelease;

    public:

        void u_clear() {
            stop();
            
            _tickCounter = 0;
            _queue.clear();
            _toRelease.clear();
        }

        friend class boost::serialization::access;
        BOOST_SERIALIZATION_SPLIT_MEMBER();

        template<class Archive>
        void save(Archive & ar, const unsigned int version) const {
            jc_assert(version == 1);
            ar & _tickCounter;
            ar & _queue;
        }

        template<class Archive>
        void load(Archive & ar, const unsigned int version) {
            ar & _tickCounter;

            switch (version) {
            case 1:
                ar & _queue;
                break;
            case 0: {
                queue_old old;
                ar & old;

                for (const auto& pair : old) {
                    auto object = _registry.u_getObject(pair.first);
                    if (object) {
                        _queue.push_back(std::make_pair(object, pair.second));
                    }
                }
                break;
            }
            default:
                jc_assert(false);
                break;
            }
        }

        explicit autorelease_queue(object_registry& registry) 
            : _tickCounter(0)
            , _registry(registry)
            // lot of dependencies :(
            , _io()
            , _work(_io)
            , _timer(_io)
        {
            _thread = std::thread([&]() { _io.run(); });
            start();
            jc_debug("aqueue created")
        }

        // prolongs object lifetime for ~10 seconds
        void prolong_lifetime(object_base& object, bool isPublic) {
            jc_debug("aqueue: added id - %u as %s", object._uid(), isPublic ? "public" : "private");

            uint32_t pushedTime = isPublic ? _tickCounter : time_subtract(_tickCounter, obj_lifeInTicks);
            spinlock::guard g(_queue_mutex);
            _queue.push_back(std::make_pair(&object, pushedTime));
        }

        // amount of objects in queue
        size_t count() {
            spinlock::guard g(_queue_mutex);
            return u_count();
        }

        queue& u_queue() {
            return _queue;
        }

        size_t u_count() const {
            return _queue.size();
        }

        // starts asynchronouos aqueue run, asynchronouosly releases objects when their time comes, starts timers, 
        void start() {
            restartTimer();
        }

        // stops async. processes launched by @start function
        void stop() {
            // will wait for @tick function execution completion
            spinlock::guard g(_timer_mutex);
            _timer.cancel();
        }

        void u_nullify() {
            for (auto &pair : _queue) {
                pair.first.jc_nullify();
            }
        }

        ~autorelease_queue() {
            // at this point of time aqueue should be empty as whole system is in half-alive-half-destroyed state
            // if not empty -> object_base::release_from_queue -> accesses died object_registry::removeObject

            stop();
            _io.stop();
            if (_thread.joinable()) {
                _thread.join();
            }
            jc_debug("aqueue destroyed")
        }

    public:
        static time_point time_subtract(time_point minuend, time_point subtrahend) {
            if (minuend >= subtrahend) {
                return minuend - subtrahend;
            }
            else {
                return (std::numeric_limits<time_point>::max)() - (subtrahend - minuend);
            }
        }

        static time_point time_add(time_point a, time_point b) {
            auto max = (std::numeric_limits<time_point>::max)();
            if ((max - a) > b) {
                return a + b;
            }
            else {
                return b - (max - a);
            }
        }

        // result is (_timeNow - time)
        time_point lifetimeDiff(time_point time) const {
            return time_subtract(_tickCounter, time);
        }

    public:

        enum {
            obj_lifetime = 10, // seconds
            tick_duration = 2, // seconds, interval between ticks, interval between aqueue tests its objects for should-be-released state,
            // and releases if needed
            one_tick = 1, // em, one tick is one tick..
        };

        enum {
            tick_duration_millis = 1000 * tick_duration, // same as tick_duration, in milliseconds
            obj_lifeInTicks = obj_lifetime / tick_duration, // object's lifetime described in amount-of-ticks
        };

    private:

        void restartTimer() {
            spinlock::guard g(_timer_mutex);
            u_startTimer();
        }

        void u_startTimer() {
            boost::system::error_code code;
            _timer.expires_from_now(boost::posix_time::milliseconds(tick_duration_millis), code);
            assert(!code);

            _timer.async_wait([&](const boost::system::error_code& e) {
                //jc_debug("aqueue code: %s - %u", e.message().c_str(), e.value());
                if (!e) { // i.e. means no error, successful completion
                    tick();
                }
            });
        }

        void tick() {
            spinlock::guard g(_timer_mutex);
            
            {
                spinlock::guard g(_queue_mutex);
                _queue.erase(
                    std::remove_if(_queue.begin(), _queue.end(), [&](const queue::value_type& val) {
                        auto diff = time_subtract(_tickCounter, val.second) + 1; // +1 because 0,1,2,3,4,5 is 6 ticks
                        bool release = diff >= obj_lifeInTicks;
                        jc_debug("id - %u diff - %u, rc - %u", val.first->_uid(), diff, val.first->refCount());

                        // just move out object reference to release it later
                        if (release)
                            _toRelease.push_back(std::move(val.first));
                        return release;
                    }),
                    _queue.end());

                // Increments tick counter, _tickCounter += 1
                _tickCounter = time_add(_tickCounter, one_tick);
            }

            jc_debug("%u objects released", _toRelease.size());

            // How much owners an object may have right now?
            // queue - +1
            // stack may reference
            // tes ..
            // Item..
            _toRelease.clear();

            u_startTimer();
        }
    };

    TEST(autorelease_queue, time_wrapping)
    {
        auto max = (std::numeric_limits<autorelease_queue::time_point>::max)();

        auto res = autorelease_queue::time_add(max, 1);
        EXPECT_TRUE(res == 1);

        res = autorelease_queue::time_add(max, 0);
        EXPECT_TRUE(res == 0);

        res = autorelease_queue::time_add(max, max);
        EXPECT_TRUE(res == max);

        res = autorelease_queue::time_add(max, 10);
        EXPECT_TRUE(res == 10);

        res = autorelease_queue::time_subtract(0, 1);
        EXPECT_TRUE(res == max - 1);

        res = autorelease_queue::time_subtract(10, 20);
        EXPECT_TRUE(res == max - 10);

        // test inversivity:
        uint32_t a = 40, b = 20;
        // if c == b - a then b == c + a and a == b - c
        uint32_t c = autorelease_queue::time_subtract(b, a);

        EXPECT_TRUE(b == autorelease_queue::time_add(a, c));
        EXPECT_TRUE(a == autorelease_queue::time_subtract(b, c));

        a = 8, b = 9;
        // c == a + b, a == c - b and b == c - a
        c = autorelease_queue::time_add(a,b);
        EXPECT_TRUE(a == autorelease_queue::time_subtract(c, b));
        EXPECT_TRUE(b == autorelease_queue::time_subtract(c, a));
    }
}

BOOST_CLASS_VERSION(collections::autorelease_queue, 1);
