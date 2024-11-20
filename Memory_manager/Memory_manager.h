#pragma once

#include <iostream>
#include <map>
#include <vector>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <windows.h>

using namespace std;

class Memory_allocator {
private:
    bool initialized = false;   // ���� �������������
     
    map<size_t, vector<void*>> pools_FSA;

    // ���� ��� Coalesce Allocation
    struct Block 
    {
        bool free;
        size_t size;
        Block* next;
        Block* prev;
    };

    Block* Head_of_list = nullptr;      // �������� ���� ��� Coalesce Allocation

    vector<void*> pools_OS;             // ������ ���������� �� ������, ���������� � ��

    //
    void merge_free_blocks(Block* block) 
    {
        if (block->prev && block->prev->free) 
        {
            block->prev->size += block->size + sizeof(Block);
            block->prev->next = block->next;

            if (block->next) {
                block->next->prev = block->prev;
            }

            block = block->prev;
        }

        if (block->next && block->next->free) 
        {
            block->size += block->next->size + sizeof(Block);
            block->next = block->next->next;

            if (block->next) {
                block->next->prev = block;
            }
        }
    }

public:
    size_t limit_to_call_os = 10 * 1024 * 1024;

    // ���������� ������ ��� ������� ������� FSA (������; ���������� ������)
    map<size_t, size_t> blocks_FSA = { {16, 10}, {32, 10}, {64, 10}, {128, 10}, {256, 10}, {512, 10} };

    size_t memory_for_coalesce = 4096;     // ����� ������ ��� Coalesce Allocation

    Memory_allocator() = default;

    ~Memory_allocator() {
        assert(!initialized && "Destroy must be called before destructor!");
        if (initialized) {
            destroy();
        }
    }

    void init()
    {
        assert(!initialized && "Allocator already initialized!");

        if (!initialized)
        {
            // ������������� FSA
            for (const auto& [blockSize, count] : blocks_FSA)
            {
                vector<void*>& pool = pools_FSA[blockSize];

                for (size_t i = 0; i < count; ++i) {
                    pool.push_back(malloc(blockSize));
                }
            }

            // ��������� ������ ��� Coalesce Allocation
            void* coalesceMemory = malloc(memory_for_coalesce);
            assert(coalesceMemory && "Failed to allocate memory for Coalesce Allocation");

            // ������� ��������� ����
            Head_of_list = static_cast<Block*>(coalesceMemory);
            Head_of_list->size = memory_for_coalesce - sizeof(Block);
            Head_of_list->free = true;
            Head_of_list->next = nullptr;
            Head_of_list->prev = nullptr;

            initialized = true;
        }
    }

    void destroy() 
    {
        assert(initialized && "Allocator not initialized!");

        // �������� FSA
        for (auto& [blockSize, pool] : pools_FSA) {
            for (void* block : pool) {
                free(block);
            }
        }

        pools_FSA.clear();

        // �������� Coalesce Allocation
        Block* current = Head_of_list;

        while (current) 
        {
            Block* next = current->next;
            free(current);
            current = next;
        }

        Head_of_list = nullptr;

        // �������� ������ ���������� ��
        for (void* osMemory : pools_OS) 
        {
            if (!VirtualFree(osMemory, 0, MEM_RELEASE)) {
                cerr << "VirtualFree failed with error: " << GetLastError() << "\n";
            }
        }
        pools_OS.clear();

        initialized = false;
    }

    void* alloc(size_t size) 
    {
        assert(initialized && "Allocator not initialized!");

        if (initialized)
        {
            size = (size + 7) & ~7; // ����������� ������ �� ������� 8 ����

            if (size == 512)
            {
                auto it = pools_FSA.find(size);

                if (it != pools_FSA.end() && !it->second.empty())
                {
                    void* block = it->second.back();
                    it->second.pop_back();

                    return block;
                }
            }
            else if (size <= limit_to_call_os)
            {
                Block* current = Head_of_list;

                while (current)
                {
                    if (current->free && current->size >= size)
                    {
                        current->free = false;

                        // ��������� ����, ���� ��� ������ ����������� ������
                        if (current->size > size + sizeof(Block)) {
                            Block* newBlock = reinterpret_cast<Block*>(
                                reinterpret_cast<char*>(current) + sizeof(Block) + size);

                            newBlock->size = current->size - size - sizeof(Block);
                            newBlock->free = true;
                            newBlock->next = current->next;
                            newBlock->prev = current;

                            if (current->next) current->next->prev = newBlock;
                            current->next = newBlock;

                            current->size = size;
                        }

                        return reinterpret_cast<void*>(current + 1);
                    }

                    current = current->next;
                }
            }
            else
            {
                void* osMemory = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

                if (!osMemory) 
                {
                    cerr << "VirtualAlloc failed with error: " << GetLastError() << "\n";
                    return nullptr;
                }

                pools_OS.push_back(osMemory);

                return osMemory;
            }
        }

        return nullptr;     // ��� ���������� �����
    }

    void free(void* ptr) 
    {
        assert(initialized && "Allocator not initialized!");

        // ���������, ����������� �� ��������� FSA
        for (const auto& [blockSize, pool] : pools_FSA) 
        {
            if (!pool.empty() && ptr >= pool.front() && ptr <= pool.back()) 
            {
                pools_FSA[blockSize].push_back(ptr);
                return;
            }
        }

        // * ��������, ��� ��������� ����������� Coalesce Allocation

        Block* block = reinterpret_cast<Block*>(reinterpret_cast<char*>(ptr) - sizeof(Block));
        block->free = true;

        // ������� ��������� ������
        merge_free_blocks(block);

        //* ��������, ��� ����������� ��
        //* ������������ ������ ��
    }

#ifdef DEBUG
    void dump_stat() const
    {
        cout << "Memory statistics:\n";


        cout << "Fixed-size Memory Allocation:\n";

        for (const auto& [blockSize, pool] : pools_FSA)
        {
            size_t totalBlocks = blocks_FSA.at(blockSize);
            size_t freeBlocks = pool.size();
            size_t occupiedBlocks = totalBlocks - freeBlocks;

            cout << "  Block size: " << blockSize
                << ", Occupied: " << occupiedBlocks
                << ", Free: " << freeBlocks << "\n";
        }


        cout << "Coalesce Allocation:\n";

        size_t coalesceFree = 0, coalesceOccupied = 0;
        Block* current = Head_of_list;

        while (current)
        {
            if (current->free) {
                coalesceFree++;
            }
            else {
                coalesceOccupied++;
            }

            current = current->next;
        }

        cout << "  Occupied: " << coalesceOccupied << ", Free: " << coalesceFree << "\n";


        cout << "OS Allocations:\n";

        cout << "  Total blocks: " << pools_OS.size() << "\n";
    }

    void dump_blocks() const
    {
        cout << "Allocated blocks:\n";


        cout << "Fixed-size Memory Allocation:\n";

        for (const auto& [blockSize, pool] : pools_FSA) 
        {
            size_t totalBlocks = blocks_FSA.at(blockSize);
            size_t occupiedBlocks = totalBlocks - pool.size();

            cout << "  Block size: " << blockSize << "\n";
            cout << "    Occupied blocks:\n";

            for (size_t i = 0; i < occupiedBlocks; ++i) {
                cout << "      Block at " << pool[i] << "\n";
            }
        }


        cout << "Coalesce Allocation:\n";

        Block* current = Head_of_list;

        while (current) 
        {
            cout << "  Block at " << current << ", size: " << current->size << ", " << (current->free ? "free" : "occupied") << "\n";
            current = current->next;
        }


        cout << "OS Allocations:\n";

        for (const auto& [size, ptr] : pools_OS) {
            cout << "  Block at " << ptr << ", size: " << size << "\n";
        }
    }
#endif
};