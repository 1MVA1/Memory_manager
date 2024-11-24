#include <iostream>
#include "Memory_manager.h"

using namespace std;

int main() 
{
    Memory_allocator allocator;

    allocator.init();
    cout << "Allocator initialized.\n";

    // Тестируем FSA
    void* ptr1 = allocator.alloc(16);
    void* ptr2 = allocator.alloc(32);
    void* ptr3 = allocator.alloc(64);

    cout << "Allocated blocks from FSA: 16 bytes, 32 bytes, 64 bytes.\n";

    allocator.free_(ptr2);
    cout << "Freed block of 32 bytes.\n";

    // Тестируем CA
    void* ptr4 = allocator.alloc(1024);
    void* ptr5 = allocator.alloc(2048);

    cout << "Allocated blocks from CA: 1024 bytes, 2048 bytes.\n";

    allocator.free_(ptr4);

    cout << "Freed block of 1024 bytes.\n";

    // Тестируем OS
    void* ptr6 = allocator.alloc(15 * 1024 * 1024); // 15 MB

    if (ptr6) 
    {
        cout << "Allocated block from OS: 15 MB.\n";
        allocator.free_(ptr6);
        cout << "Freed block of 15 MB.\n";
    } else {
        cout << "OS allocation failed for 15 MB.\n";
    }

//#ifdef DEBUG
    allocator.dump_stat();
    allocator.dump_blocks();
//#endif

    allocator.destroy();
    cout << "Allocator destroyed.\n";

    return 0;
}
