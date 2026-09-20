#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{

    // TODO: wait, obtain mutex, wait, release mutex as described by thread_data structure
    // hint: use a cast like the one below to obtain thread arguments from your parameter
    //struct thread_data* thread_func_args = (struct thread_data *) thread_param;
    struct thread_data* thread_func_args = (struct thread_data*) thread_param;
    struct timespec wait_to_obtain;
    struct timespec wait_to_release;
    wait_to_obtain.tv_sec=thread_func_args->wait_to_obtain/1000;
    wait_to_obtain.tv_nsec=(thread_func_args->wait_to_obtain%1000)*1000000;
    wait_to_release.tv_sec=thread_func_args->wait_to_release/1000;
    wait_to_release.tv_nsec=(thread_func_args->wait_to_release%1000)*1000000;;
    int rc;
    //wait a given amount before attempting to obtain mutex
    nanosleep(&wait_to_obtain,NULL);

    //obtain mutex
    rc = pthread_mutex_lock(thread_func_args->mtx);
    if (rc!=0){
	    ERROR_LOG("Failed to acquire Mutex!");
	    thread_func_args->thread_complete_success=false;
    }
    else{
	    DEBUG_LOG("Successfully acquired Mutex!");
    }
    nanosleep(&wait_to_release,NULL);
    rc = pthread_mutex_unlock(thread_func_args->mtx);
    if(rc!=0){
	    ERROR_LOG("Failed top unlock Mutext!");
	    thread_func_args->thread_complete_success=false;
    }
    else{
	    DEBUG_LOG("Successfully unlocked Mutex!");
	    thread_func_args->thread_complete_success=true;
    }
    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    /**
     * TODO: allocate memory for thread_data, setup mutex and wait arguments, pass thread_data to created thread
     * using threadfunc() as entry point.
     *
     * return true if successful.
     *
     * See implementation details in threading.h file comment block
     */


    struct thread_data *thread_param = malloc(sizeof(*thread_param));
    if(thread_param==NULL){ERROR_LOG("failed to allocate thread parameter memory"); return false;}
    
    thread_param->mtx=mutex;
    thread_param->wait_to_obtain=wait_to_obtain_ms;
    thread_param->wait_to_release=wait_to_release_ms;

    //pthread_attr_t attr;
    int rc = pthread_create(thread, NULL, threadfunc, thread_param);
    if (rc!=0){
	    ERROR_LOG("failed to start thread");
	    return false;
    }
    return true;
 
}

