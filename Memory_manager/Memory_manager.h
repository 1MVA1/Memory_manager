#pragma once

//#ifdef DEBUG
#include <iostream>
//#endif

#include <cassert>  
#include <array>
#include <windows.h>    // ��� ������� VirtualAlloc � VirtualFree ��� ������ � ������� ��

using namespace std;

// ������������� �������� ������, ���������� � ����� ��������� � ���������� �������: Fixed-Size Allocator (FSA), 
// Coalesced Allocator (CA) � ���������� ������ � ������������ ������� (OS allocation).
//
// Fixed-Size Allocator (FSA):
// - �������� ��������� � ������������ ������ ����������� �� O(1) ��������� ������������� ������������ ������.
// - ��������� ���������� ������������ �� ���� �������������� ������� ������.
// - ������� ������������ ������ �� ������ �� ������������������ � ����� ���������������� ����������.
//
// Coalesced Allocator (CA):
// - ��������� ������� ��� ������ ����������� �������, ������� ������ ��������� ������� (limit_to_call_os).
// - ���������� ������� ������� ���������� ���� ������ (memory_for_coalesce).
// - ��������� ����� �������� � ���������� ������, ��� �������� ���������� � �����������.
// - ��� ������������ �������� ��������� ����� ������������� ������������, �������� ������������.
// - ���� ���������� ������ ������ ���������� �����, ���������� ������ ���������� ����� ��������� ������.
//
// OS Allocation:
// - ������������ ������� ������ ��� ������� ������, ����������� �������� ����� (limit_to_call_os).
// - ������ ������ ������ �������������� �������������, ��� ����������� ��� ���������� ������.
// - ����� ������ ����� �������� �� �������, ������� ����������� ������ ��� ������� ��������.    
class Memory_allocator
{
private:
    bool is_init = false;

    void* ptr_main = nullptr;

    struct FSA_Pool 
    {
        size_t size;
        size_t count;
        void* head; 

        // ����������� ��� ������� �������������
        FSA_Pool(size_t block_size, size_t count) : size(block_size), count(count) {}
    };

    array<FSA_Pool, 6> pools_FSA = { {
        FSA_Pool(16, 10),
        FSA_Pool(32, 10),
        FSA_Pool(64, 10),
        FSA_Pool(128, 10),
        FSA_Pool(256, 10),
        FSA_Pool(512, 10)
    } };

    struct Block_FSA {
        Block_FSA* next;
    };

    struct Block_CA
    {
        bool is_free;      
        size_t size;   
        Block_CA* next;
        Block_CA* prev;
    };

    Block_CA* head_CA = nullptr; 
    size_t memory_CA = 4096; 

    struct Block_OS
    {         
        size_t size;
        Block_OS* next;
    };

    Block_OS* head_OS = nullptr;
    size_t limit_to_call_os = 10 * 1024 * 1024;

    // ����� ����������� �������� ��������� ������ � CA
    // ���� ���������� ��� ��������� ����� ��������, ��� ������������ � ������� ��� ����������� ������������
    void merge_free_blocks(Block_CA* block)
    {
        // ���� ���������� ���� ��������, ���������� ��� � �������
        if (block->prev && block->prev->is_free)
        {
            // ����������� ������ ����������� �����
            block->prev->size += block->size + sizeof(Block_CA); 
            // ���������� ������� ���� � ������
            block->prev->next = block->next; 

            if (block->next)
            {
                // ��������� ��������� �� ���������� � ���������� �����
                block->next->prev = block->prev; 
            }

            // ������� ������� ���� �� ����������
            block = block->prev;
        }

        // ���� ��������� ���� ��������, ���������� ��� � ������� (������ �����������)
        if (block->next && block->next->is_free)
        {
            block->size += block->next->size + sizeof(Block_CA);
            block->next = block->next->next;

            if (block->next)
            {
                block->next->prev = block;
            }
        }
    }

public:
    Memory_allocator() = default;

    ~Memory_allocator()
    {
        assert(!is_init && "Destroy must be called before destructor!");

        if (is_init)
        {
            destroy();
        }
    }

    // �������������� ������ ��� FSA � CA, ������� ����� ����
    void init()
    {
        assert(!is_init && "Allocator already initialized!");

        if (is_init) {
            return;
        }

        // ������������ ����� ����� ������, ����������� ��� FSA � CA
        size_t memory_FSA = 0;

        for (const auto& pool : pools_FSA) {
            memory_FSA += (sizeof(FSA_Pool) + (pool.size + sizeof(Block_FSA)) * pool.count);
        }

        // nullptr � ����� ������ ����������� ����� ������. ���� �������� nullptr, ������� ���� ������� ���������� �����
        // total_size � ������ ������ � ������, ������� ����� ��������. ������ ����������� �� �������� ������� �������� (������ 4 ��)
        // MEM_RESERVE � ����������� ����������� ������ ��� ��������� ���������� ������� ������
        // ��� ������ �� �������� �� ��� ���, ���� �� ����� "����������" ����� MEM_COMMIT
        // MEM_COMMIT � �������� ���������� �������� ������ � ��������� �� � ����������� �������
        // PAGE_READWRITE � ������������� ����� ������� �� ������: ������ � ������ ��������
        // ���������� ��������� �� ������ ����������� �������
        ptr_main = VirtualAlloc(nullptr, memory_FSA + memory_CA, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

        // ��������� ������ �� �����: FSA � CA
        // uint8_t � ��� ��� ������������ ������ ����� �������� 1 ���� (8 ���), ������� �������� �������� ��� ������ � �������� � ���������� � ������
        // ������������� uint8_t* ��������� ��������� �������������� �������� � ����������� �� ������ ��������� ������
        uint8_t* current_ptr = reinterpret_cast<uint8_t*>(ptr_main);

        // ������������� FSA
        for (auto& pool : pools_FSA)
        {
            pool.head = current_ptr;

            // ��������� ������ ������ ���� �� �����
            // ��������� ���� ��������� �� nullptr
            Block_FSA* block = static_cast<Block_FSA*>(pool.head);

            for (size_t i = 0; i < pool.count - 1; ++i)
            {
                // �������� ��������� � ���� Block_FSA � ������� reinterpret_cast
                // reinterpret_cast ��������� ��������� �������������� �������������� �����
                block->next = reinterpret_cast<Block_FSA*>(reinterpret_cast<uint8_t*>(block) + pool.size + sizeof(Block_FSA));
                block = block->next;
            }

            current_ptr += (pool.size + sizeof(Block_FSA)) * pool.count;
        }

        // ������������� CA
        head_CA = reinterpret_cast<Block_CA*>(current_ptr);
        // ��������� ������ ��������� ������
        head_CA->size = memory_CA - sizeof(Block_CA);
        head_CA->is_free = true;
        head_CA->next = nullptr;
        head_CA->prev = nullptr;

        is_init = true;
    }

    // ���������� ���������, ���������� ��� ���������� ������
    void destroy()
    {
        assert(is_init && "Allocator not initialized!");

        // ptr_main - ��������� �� ������ ������� ������, ������� ����� ����������
        // MEM_RELEASE - ���������, ��� ����� ���������� ���� ������ ������ (������� 2 �������� ������������, ����� ��������� ���� 0)
        VirtualFree(ptr_main, 0, MEM_RELEASE);

        for (auto& pool : pools_FSA) {
            pool.head = nullptr;
        }

        head_CA = nullptr;

        Block_OS* current_OS = head_OS;

        while (current_OS)
        {
            Block_OS* next = current_OS->next;

            VirtualFree(current_OS, 0, MEM_RELEASE);

            current_OS = next;
        }

        head_OS = nullptr;

        is_init = false;
    }

    // �������� ���� ������ ��������� �������
    // FSA: ���������� ���� �� ���� �������������� �������
    // CA: ���������� ���� �� ������������ ������
    // OS: �������� ���� ����� VirtualAlloc
    void* alloc(size_t size)
    {
        assert(is_init && "Allocator not initialized!");

        // ������������ ������� �� ������� 8 ����
        size = (size + 7) & ~7; 

        // ���� ������ ������������� FSA, �������� ����� ���� �� ����
        if (size <= pools_FSA.back().size) 
        {
            for (auto& pool : pools_FSA) 
            {
                if (size <= pool.size) 
                {
                    if (pool.head) 
                    {
                        Block_FSA* block = static_cast<Block_FSA*>(pool.head);
                        pool.head = block->next;  // ���������� ������ �� ��������� ����

                        // block + 1 ������������, ����� �������� ��������� �� ������, ������� ��������� ����� ���������� �����
                        return reinterpret_cast<void*>(block + 1);
                    }
                }
            }
        }
        // ���� ������ �������� ��� CA
        else if (size <= limit_to_call_os)
        {
            Block_CA* current = head_CA;

            while (current)
            {
                if (current->is_free && current->size >= size)
                {
                    current->is_free = false;

                    // ��������� �����, ���� ������� ���������� �����
                    if (current->size > size + sizeof(Block_CA))
                    {
                        // �������� ��������� � ���� Block_CA � ������� reinterpret_cast
                        // reinterpret_cast ��������� ��������� �������������� �������������� �����
                        Block_CA* new_block = reinterpret_cast<Block_CA*>(reinterpret_cast<char*>(current) + sizeof(Block_CA) + size);

                        // ��������� ������ ������ �����
                        new_block->size = current->size - size - sizeof(Block_CA);
                        new_block->is_free = true;
                        new_block->next = current->next;
                        new_block->prev = current;

                        if (current->next)
                        {
                            current->next->prev = new_block;
                        }

                        current->next = new_block;
                        current->size = size;
                    }

                    // current + 1 ������������, ����� �������� ��������� �� ������, ������� ��������� ����� ���������� �����
                    return reinterpret_cast<void*>(current + 1);
                }

                current = current->next;
            }
        }
        else
        {
            // ���� ������ ��������� ����� CA, �������� ������ � ��
            void* ptr = VirtualAlloc(nullptr, size + sizeof(Block_OS), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

            Block_OS* block = static_cast<Block_OS*>(ptr);
            block->size = size;

            if (head_OS == nullptr) {
                head_OS = block;
            }
            else 
            {
                Block_OS* current = head_OS;

                while (current->next) {
                    current = current->next;
                }

                current->next = block;
            }

            return ptr;
        }

        // ���� ������ �� ������� ��������
        return nullptr;
    }
 
    // ����������� ���� ������:
    // FSA: ���������� ���� � ���
    // CA: �������� ���� ��� ��������� � ���������� ��� � ���������
    // OS: ����������� ������ ����� VirtualFree
    void free_(void* ptr)
    {
        assert(is_init && "Allocator not initialized!");

        if (!ptr)
        {
            return;
        }

        for (auto& pool : pools_FSA) 
        {
            // �������� �������������� � FSA
            if (ptr >= pool.head && ptr < reinterpret_cast<void*>(reinterpret_cast<char*>(pool.head) + pool.count * (pool.size + sizeof(Block_FSA)))) 
            {
                Block_FSA* block = static_cast<Block_FSA*>(ptr);
                block->next = static_cast<Block_FSA*>(pool.head);
                pool.head = block;

                return;
            }
        }

        // �������� �������������� � CA
        if (ptr > head_CA && ptr < reinterpret_cast<void*>(reinterpret_cast<char*>(head_CA) + memory_CA))
        {
            Block_CA* block = reinterpret_cast<Block_CA*>(reinterpret_cast<char*>(ptr) - sizeof(Block_CA));     // �������� �������������� �����

            if (!block->is_free)
            {
                block->is_free = true;
                merge_free_blocks(block);
            }

            return;
        }

        Block_OS* prev = nullptr;
        Block_OS* current = head_OS;

        while (current) 
        {
            // �������� �������������� � OS
            if (reinterpret_cast<void*>(current + 1) == ptr) 
            {
                if (prev) {
                    prev->next = current->next;
                }
                else {
                    head_OS = current->next;
                }

                VirtualFree(current, 0, MEM_RELEASE);

                return;
            }

            prev = current;
            current = current->next;
        }
    }

//#ifdef DEBUG
    // ����� ���������� ������� � ��������� ������ ��� CA � ��
    // ��� FSA ���� �������� ������ ������� ��� ������� �������, �� ��� ���� ���
    void dump_stat() const
    {
        cout << "\nMemory statistics:\n****************************************\n\n";

        cout << "----------------------------------------\n\nCoalesce Allocation:\n----------------------------------------\n";

        size_t occupied_blocks = 0;
        size_t free_blocks = 0;

        Block_CA* current_CA = head_CA;

        while (current_CA)
        {
            if (current_CA->is_free) {
                free_blocks++;
            }
            else {
                occupied_blocks++;
            }

            current_CA = current_CA->next;
        }

        cout << "Free: " << free_blocks << ", Occupied: " << occupied_blocks << "\n";

        cout << "----------------------------------------\n\nOS Allocations:\n----------------------------------------\n";

        occupied_blocks = 0;

        Block_OS* current = head_OS;

        while (current)
        {
            ++occupied_blocks;

            current = current->next;
        }

        cout << "Occupied: " << occupied_blocks << "\n";

        cout << "----------------------------------------\n";

        cout << "\n****************************************\n";
    }

    // ����� ������ � ������� ���� ���������� ������ ��� FSA, CA � OS
    void dump_blocks() const
    {
        cout << "\nAllocated blocks:\n****************************************\n\n";

        cout << "Fixed-size Memory Allocation:\n----------------------------------------\n";

        for (const auto& pool : pools_FSA)
        {
            size_t free_blocks = 0;
            Block_FSA* block = static_cast<Block_FSA*>(pool.head);

            while (block)
            {
                cout << "Block at " << block << ", size: " << pool.size << "\n";

                block = block->next;
            }
        }

        cout << "----------------------------------------\n\nCoalesce Allocation:\n----------------------------------------\n";

        Block_CA* current_CA = head_CA;

        while (current_CA)
        {
            if (!current_CA->is_free) {
                cout << "Block at " << current_CA << ", size: " << current_CA->size << "\n";
            }
            current_CA = current_CA->next;
        }

        cout << "----------------------------------------\n\nOS Allocations:\n----------------------------------------\n";

        Block_OS* current_OS = head_OS;

        while (current_OS)
        {
            cout << "Block at " << current_OS << ", size: " << current_OS->size << "\n";

            current_OS = current_OS->next;
        }

        cout << "----------------------------------------\n";

        cout << "\n****************************************\n";
    }
//#endif
};