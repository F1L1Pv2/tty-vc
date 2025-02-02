#include <stdio.h>
#include <stdint.h>

#include <coroutine.h>

void counter(void* arg){
    for(size_t i = 0; i < (size_t)arg; i++){
        printf("[%ld] %lu\n",coroutine_id(),i);
        coroutine_yield();
    }
}

int main(){
    coroutine_init();

    coroutine_go(counter, (void*)5);
    coroutine_go(counter, (void*)10);

    while(coroutine_alive() > 1){
        coroutine_yield();
    }

    coroutine_finish();
    return 0;
}