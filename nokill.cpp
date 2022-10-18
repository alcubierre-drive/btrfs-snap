#include <signal.h>
#include <thread>
#include <mutex>
#include <cstdio>

#define KILL_SECONDS_MAX 5
#define KILL_SECONDS_NUM 5

static std::mutex kill_mutex;
static std::thread* kill_thread;
static bool thread_exit = false;

static double seconds_passed = 0.0;
static int times_pressed = 0;

static void kill_program( double nsec, int ntim ) {
    fprintf(stderr, "\nSIGINT %i times in %i seconds. exiting.\n",
            ntim+1, (int)(nsec+0.5) );
    thread_exit = true;
    exit(1);
}

static void print_kill_info() {
    fprintf(stderr, "\nto kill program, press Ctrl+C %i times in %i seconds\n",
            KILL_SECONDS_NUM+1, KILL_SECONDS_MAX);
}

static void kill_function( int signum ) {
    if (signum != SIGINT) return;
    kill_mutex.lock();
    if ((int)(seconds_passed+0.5) < KILL_SECONDS_MAX) {
        if (times_pressed < KILL_SECONDS_NUM) {
            if (!times_pressed) print_kill_info();
            times_pressed++;
        } else {
            kill_program( seconds_passed, times_pressed );
        }
    } else {
        seconds_passed = 0.0;
        times_pressed++;
    }
    kill_mutex.unlock();
}

void nokill_init() {
    kill_mutex.unlock();
    kill_thread = new std::thread( [&](){
        while (!thread_exit) {
            usleep(1.e+5);
            if (times_pressed) {
                kill_mutex.lock();
                seconds_passed+=0.1;
                kill_mutex.unlock();
            }
        }
    } );
    signal(SIGINT, kill_function);
}

void nokill_clear() {
    thread_exit = true;
    kill_thread->join();
}

