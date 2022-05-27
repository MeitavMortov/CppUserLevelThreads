/**
 * This file is part of project 2 in OS course.
 * Contains the implementation of the user-level threads library.
 * @authers: Zimrat Kniel, Meitav Ovadya-Mortov.
*/

#include <iostream>
#include "uthreads.h"
#include <deque>
#include <unordered_map>
#include <map>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <algorithm>
#include <vector>

#define ERROR_SIGEMPTYSET "system error: sigemptyset failed\n"
#define ERROR_ALLOCATION "system error: allocation failed\n"
#define ERROR_SIGPROMASK "system error: sigprocmask failed\n"
#define ERROR_SETTIMER "system error: set timer failed\n"
#define ERROR_SIGADDSET "system error: sigaddset failed\n"
#define ERROR_SIGACTION "system error: sigaction failed\n"
#define ERROR_INVALID_QUANTUMS "thread library error: invalid quantum_usecs value\n"
#define ERROR_INVALID_NUM_OF_THREADS "thread library error: number of threads exceeds the limit\n"
#define ERROR_INVALID_ENTRY_POINT "thread library error: thread entry point is nullptr\n"
#define ERROR_INVALID_ID "thread library error: thread id does not exist\n"
#define ERROR_TRY_MAIN_BLOCK "thread library error: tried blocking the main thread\n"

#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%fs:0x30,%0\n"
                 "rol    $0x11,%0\n"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor    %%gs:0x18,%0\n"
                 "rol    $0x9,%0\n"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}
#endif

/**
 * enum State represents the state of the thread. the state can be: ready / running / blocked.
*/
enum State {ready, running, blocked };

/**
 * Inner class called thread, represents the user thread object in the user-level thread library.
*/
class Thread{
public:
    //Thread's fields:
    int id; //id of the thread
    int is_main; //boolean represents the answer to the question: "Is this thread the main thread?"
    int is_sleeping; //boolean represents the answer to the question: "Is this thread sleeping now?"
    enum State state; //state of the thread.
    sigjmp_buf env; //env of the thread.
    int t_quantums_counter; //counter of number of quantums the thread was in running state.
    char* stack; //the dynamically allocated stack of the thread.

    /**
     * Constructor
     * Constructs new Thread object and inits its fields.
     * @param: id id of the new thread.
     * @param: entery_point thread_entry_point of the new thread.
     * @param: is_main answer if new thread is the main thread.
    */
    Thread(int id, thread_entry_point entry_point, int is_main){
        this->id = id;
        this->is_main = is_main;
        this->is_sleeping = 0;
        if (is_main){ // inits fields of main thread.
            this->stack = nullptr;
            this->state = running;
            this->t_quantums_counter = 1;
            sigsetjmp(env, 1);
            if(sigemptyset(&env->__saved_mask) == -1){
                std::cerr << ERROR_SIGEMPTYSET;
                exit(1);
            }
        }
        else{ //inits fields if thread is not the main thread.
            try {
                this->stack = new char[STACK_SIZE];
            }
            catch(const std::bad_alloc& e) {
                std::cerr << ERROR_ALLOCATION;
                exit(1);
            }
            this->t_quantums_counter = 0;
            this->state = ready;
            setup_thread(entry_point);
        }
    }

    /**
     * Setup env field of a thread.
     * Sets up the sigjmp_buf of the thread which called env field.
     * env is used siglongjmp to jump into the thread.
     * @param: entery_point thread_entry_point of the new thread.
    */
    void setup_thread(thread_entry_point entry_point)
    {
        address_t sp = (address_t) stack + STACK_SIZE - sizeof(address_t);
        address_t pc = (address_t) entry_point;
        sigsetjmp(env, 1);
        (env->__jmpbuf)[JB_SP] = translate_address(sp);
        (env->__jmpbuf)[JB_PC] = translate_address(pc);
        if(sigemptyset(&env->__saved_mask) == -1){
            std::cerr << ERROR_SIGEMPTYSET;
            exit(1);
        }
    }

    /**
     * Destructor
     * Destructs Thread object and free its stack.
    */
    ~Thread(){
        delete[] this->stack;
    }
};

/**
 * Define global variables for the user threads management:
 */
    //deque contains the ready threads, deque manages the fifo principle:
    std::deque<int> ready_queue;

    //Map id to pointer to thread. Map contains all the threads that were created.
    std::map<int, Thread*> threads_map;

    //Map id to the value of quantums_counter that the thread of this id needs to finish sleeping.
    // Map contains all the threads that (is_sleeping == 1).
    std::unordered_map<int, int> sleep_map;

    //Length of a quantum in micro-seconds:
    int quantum_usecs;

    //General counter of the quantums during the program:
    int quantums_counter=1;

    //Sigaction object contains what should be the program's behavior when receiving timer signals.
    struct sigaction sa = {0};

    //The general timer for the program:
    struct itimerval timer;

    //Contains the id of the running thread:
    int running_thread;

    //Set of OS signals the program should block.
    sigset_t blocked_set;



/**
 * Restart_timer
 * Restart timer when is needed.
*/
void restart_timer(){
    if(setitimer(ITIMER_VIRTUAL, &timer, NULL) == -1){
        std::cerr << ERROR_SETTIMER;
        exit(1);
    }
}

/**
 * Function that choose id for a new thread.
 * we assume that the map is not full and we can create a new thread.
 * finds the smallest valid id ( using for loop on the map keys).
 * Restart timer when is needed.
 * returns chosen id
*/
int choose_id(){
    for(int i=1; i<MAX_THREAD_NUM; i++){
        if ( threads_map.find (i) == threads_map.end())
        {
            return i;
        }
    }
    return -1;
}

/**
 * Jump to thread with the id was given as an argument.
 * @param: tid_next_thread id of the thread to jump into.
*/
    void jump_to_thread(int tid_next_thread)
    {
        running_thread = tid_next_thread;
        siglongjmp(threads_map[tid_next_thread]->env, 1);
    }
/**
 * Function that saves the "bookmark" of the running thread and jump to the thread with the given id if needed.
 * @param: tid_next_thread id of the thread to jump into.
*/
   void yield(int tid_next_thread)
    {
        int ret_val = sigsetjmp(threads_map[running_thread]->env, 1);
        bool did_just_save_bookmark = ret_val == 0;
        if (did_just_save_bookmark)
        {
            jump_to_thread(tid_next_thread);
        }
        else{
            if(sigprocmask(SIG_UNBLOCK, &blocked_set, NULL) == -1){
                std::cerr << ERROR_SIGPROMASK;
                exit(1);
            }
        }
    }

/**
* Function that wake up threads that needs to stop sleep.
*/
    void wake_up_threads(){
        //saves all the keys that have to waked up.
        std::vector<int> keys_to_wake_up;
        for(auto const& item : sleep_map){
            if (item.second == quantums_counter){
                keys_to_wake_up.push_back(item.first);
            }
        }
        //make all changes that have to wake up:
        for(auto  const& key : keys_to_wake_up){
            threads_map[key]->is_sleeping = 0;
            //push  deque if not blocked:
            if(threads_map[key]->state != blocked){
                ready_queue.push_back(key);
            }
            sleep_map.erase(key);
        }
    }

    /**
     * Function that run the next available thread.
     * @param: is_sent_from_scheduler boolean represents the answer of question: is schduler called run thread?.

    */
    void run_thread(int is_sent_from_scheduler){
        if (sigprocmask(SIG_BLOCK, &blocked_set, NULL) == -1){
            std::cerr << ERROR_SIGPROMASK;
            exit(1);
        }
        if(!is_sent_from_scheduler){
            wake_up_threads(); // new!!
        }
        int tid = ready_queue.front();
        ready_queue.pop_front();
        threads_map[tid]->state=running;
        threads_map[tid]->t_quantums_counter++;
        quantums_counter++;
        restart_timer();
        if (threads_map.find (running_thread) == threads_map.end()){
           jump_to_thread(tid);
        }
        else{
           yield(tid);
        }
    }

/**
* Function that deletes thread with id == tid.
* @param: tid id of the thread to delete.
*/
    void delete_thread(int tid){
        if(running_thread == tid){
           threads_map.erase(tid);
            run_thread(0);
        }
        else{ //thread is not the running thread
            if(threads_map[tid]->is_sleeping){
                sleep_map.erase(tid);
            }
            threads_map.erase(tid); //remove from map if it's there
            auto tid_iterator = std::find(ready_queue.begin(), ready_queue.end(),tid);
            if(tid_iterator!=ready_queue.end()){
                ready_queue.erase(tid_iterator); //remove from ready if its there
            }
        }
    }

/**
* Function that blocks thread with id == tid.
* @param: tid id of the thread to block.
*/
    void block_thread(int tid){
        //change state to block
        if (threads_map[tid]->state == blocked){
            return;
        }
        if(sigprocmask(SIG_BLOCK, &blocked_set, NULL) == -1){
            std::cerr << ERROR_SIGPROMASK;
            exit(1);
        }
        threads_map[tid]->state = blocked;
        if(tid == running_thread){
            run_thread(0);
        }
        else{
            auto tid_iterator = std::find(ready_queue.begin(), ready_queue.end(),tid);
            if(tid_iterator != ready_queue.end()){
                ready_queue.erase(tid_iterator);
            }
        }
        if(sigprocmask(SIG_UNBLOCK, &blocked_set, NULL) == -1){
            std::cerr << ERROR_SIGPROMASK;
            exit(1);
        }
    }

/**
* Function that resumes thread with id == tid.
* @param: tid id of the thread to resumes.
*/
    void resume_thread(int tid){
        //change mode of thread to ready
        if (threads_map[tid]->state != blocked){
            return;
        }
        threads_map[tid]->state = ready;
        if(!(threads_map[tid]->is_sleeping)){
            ready_queue.push_back(tid);  //insert it to ready queue
        }
    }

/**
* Function that causes thread with id (== tid) to sleep.
* @param: tid id of the thread that has to sleep.
*/
    void sleep_thread(int num_quantums){
    if(sigprocmask(SIG_BLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }
    //change is_sleeping:wake_up_threads
    threads_map[running_thread]->is_sleeping = 1;
    //change state to ready:
    threads_map[running_thread]->state = ready;
    // add to sleep map:
    sleep_map[running_thread]= quantums_counter + num_quantums;
    if(sigprocmask(SIG_UNBLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }
    //run thread:
    run_thread(0);
}

/**
* Function that deal with a signal from the timer more details in the function itself.
* @param: sig_num
*/
    void scheduler(int sig_num){
        if(sigprocmask(SIG_BLOCK, &blocked_set, NULL) == -1){
            std::cerr << ERROR_SIGPROMASK;
            exit(1);
        }
        threads_map[running_thread]->state = ready; //chang state
        wake_up_threads(); // new!!
        ready_queue.push_back(running_thread); //push to ready queue
        run_thread(1); //helper function that run next thread, for more details goto its documentation.
    }


/**
* Function that init all the global variables for the user threads management:
* @param: quantum_usecs the length of a quantum in micro-seconds.
*/
    void init(int quantum_usecs){
        quantum_usecs = quantum_usecs;
        running_thread = 0;
        timer.it_value.tv_sec = 0;
        timer.it_value.tv_usec = quantum_usecs;
        timer.it_interval.tv_sec = 0;
        timer.it_interval.tv_usec = 0;

        if(sigemptyset(&blocked_set) == -1){
            std::cerr << ERROR_SIGEMPTYSET;
            exit(1);
        }
        if(sigaddset(&blocked_set, SIGVTALRM) == -1){
            std::cerr << ERROR_SIGADDSET;
            exit(1);
        }
        sa.sa_handler = &scheduler;
        if(sigaction(SIGVTALRM, &sa, nullptr) == -1){
            std::cerr << ERROR_SIGACTION;
            exit(1);
        }
        Thread* thread;
        try {
            thread = new Thread(0, nullptr, 1);
        }
        catch(const std::bad_alloc& e) {
            std::cerr << ERROR_ALLOCATION;
            exit(1);
        }
        restart_timer();
        threads_map[0] = thread;
    }


/*
 * From here is the implementation of the uthreads library function:
 */

/**
 * @brief initializes the thread library.
 *
 * You may assume that this function is called before any other thread library function, and that it is called
 * exactly once.
 * The input to the function is the length of a quantum in micro-seconds.
 * It is an error to call this function with non-positive quantum_usecs.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_init(int quantum_usecs){
    if(quantum_usecs <= 0){
        std::cerr << ERROR_INVALID_QUANTUMS;
        return -1;
    }
    init(quantum_usecs);
    return 0;
}

/**
 * @brief Creates a new thread, whose entry point is the function entry_point with the signature
 * void entry_point(void).
 *
 * The thread is added to the end of the READY threads list.
 * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
 * limit (MAX_THREAD_NUM).
 * Each thread should be allocated with a stack of size STACK_SIZE bytes.
 *
 * @return On success, return the ID of the created thread. On failure, return -1.
*/
int uthread_spawn(thread_entry_point entry_point){
    if(threads_map.size() >= MAX_THREAD_NUM){
        std::cerr << ERROR_INVALID_NUM_OF_THREADS;
        return -1;
    }
    if(entry_point == nullptr){
        std::cerr << ERROR_INVALID_ENTRY_POINT;
        return -1;
    }
    if(sigprocmask(SIG_BLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }
    int new_id = choose_id();
    Thread* thread;
    try{
        thread = new Thread(new_id, entry_point, 0);
    }
    catch(const std::bad_alloc& e) {
        std::cerr << ERROR_ALLOCATION;
        exit(1);
    }
    threads_map[new_id] = thread;
    ready_queue.push_back(new_id);
    if(sigprocmask(SIG_UNBLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }
    return new_id;
}

/**
 * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
 *
 * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
 * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
 * BLOCKED state has no effect and is not considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_block(int tid){
    if(threads_map.find(tid) == threads_map.end()){
        std::cerr << ERROR_INVALID_ID;
        return  -1;
    }
    if(tid == 0){
        std::cerr << ERROR_TRY_MAIN_BLOCK;
        return  -1;
    }
    if(sigprocmask(SIG_BLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }
    block_thread(tid);
    if(sigprocmask(SIG_UNBLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }
    return 0;
}

/**
 * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
 *
 * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
 * ID tid exists it is considered an error.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_resume(int tid){
    if(threads_map.find(tid)==threads_map.end()){
        std::cerr << ERROR_INVALID_ID;
        return  -1;
    }
    if(sigprocmask(SIG_BLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }
    resume_thread(tid);
    if(sigprocmask(SIG_UNBLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }
    return 0;
}

/**
 * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
 *
 * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
 * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
 * process using exit(0) (after releasing the assigned library memory).
 *
 * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
 * itself or the main thread is terminated, the function does not return.
*/
int uthread_terminate(int tid){
    if(sigprocmask(SIG_BLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }
    if(tid == 0){
        threads_map.clear();
        exit(0);
    }
    if(threads_map.find(tid)==threads_map.end()){
        std::cerr << ERROR_INVALID_ID;

        if(sigprocmask(SIG_UNBLOCK, &blocked_set, NULL) == -1){
            std::cerr << ERROR_SIGPROMASK;
            exit(1);
        }
        return  -1;
    }
    else{
        delete_thread(tid);
         if(sigprocmask(SIG_UNBLOCK, &blocked_set, NULL) == -1){
             std::cerr << ERROR_SIGPROMASK;
             exit(1);
         }
    }
    return 0;
}

/**
 * @brief Returns the thread ID of the calling thread.
 *
 * @return The ID of the calling thread.
*/
int uthread_get_tid(){
    return running_thread;
}

/**
 * @brief Returns the total number of quantums since the library was initialized, including the current quantum.
 *
 * Right after the call to uthread_init, the value should be 1.
 * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
 *
 * @return The total number of quantums.
*/
int uthread_get_total_quantums(){
    return quantums_counter;
}

/**
 * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
 *
 * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
 * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
 * also the current quantum). If no thread with ID tid exists it is considered an error.
 *
 * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
*/
int uthread_get_quantums(int tid){
    if(sigprocmask(SIG_BLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }
    if(threads_map.count(tid) != 1){
        std::cerr << ERROR_INVALID_ID;

        if(sigprocmask(SIG_UNBLOCK, &blocked_set, NULL) == -1){
            std::cerr << ERROR_SIGPROMASK;
            exit(1);
        }

        return  -1;
    }
    if(sigprocmask(SIG_UNBLOCK, &blocked_set, NULL) == -1){
        std::cerr << ERROR_SIGPROMASK;
        exit(1);
    }

    return threads_map[tid]->t_quantums_counter;
}

/**
 * @brief Blocks the RUNNING thread for num_quantums quantums.
 *
 * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
 * After the sleeping time is over, the thread should go back to the end of the READY threads list.
 * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
 * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
 * It is considered an error if the main thread (tid==0) calls this function.
 *
 * @return On success, return 0. On failure, return -1.
*/
int uthread_sleep(int num_quantums){
    if(running_thread == 0){
        std::cerr << ERROR_TRY_MAIN_BLOCK;
        return  -1;
    }
    if(num_quantums <= 0){
        std::cerr << ERROR_INVALID_QUANTUMS;
        return  -1;
    }
    sleep_thread(num_quantums);
    return 0;
}
