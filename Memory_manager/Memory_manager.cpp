﻿#include "Memory_manager.h"

int main() 
{
    Memory_allocator allocator;

    allocator.init();
    int* pi = (int*)allocator.alloc(sizeof(int));
    double* pd = (double*)allocator.alloc(sizeof(double));
    int* pa = (int*)allocator.alloc(10 * sizeof(int));

//#ifdef DEBUG
    allocator.dump_stat();
    allocator.dump_blocks();
//#endif

    allocator.free_(pa);
    allocator.free_(pd);
    allocator.free_(pi);

    allocator.destroy();

    return 0;
}
