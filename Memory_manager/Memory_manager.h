#pragma once

#include <iostream>
#include <map>
#include <vector>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <windows.h>

using namespace std;

class Memory_allocator
{
private:
    bool is_int = false;

    map<size_t, size_t> blocks_FSA = { {16, 10}, {32, 10}, {64, 10}, {128, 10}, {256, 10}, {512, 10} };
    map<size_t, vector<void*>> pools_FSA;

    // ��������� �������� ����� ��� CA
    struct Block_CA
    {
        bool is_free;      
        size_t size;   
        Block_CA* next;  
        Block_CA* prev; 
    };

    Block_CA* head_of_list = nullptr; 
    size_t memory_for_coalesce = 4096; 

    // ��������� ��� �������� �����, ����������� � ��
    struct Block_OS
    {
        void* ptr;         
        size_t size;        
    };

    size_t limit_to_call_os = 10 * 1024 * 1024;
    vector<Block_OS> pools_OS;

    // ����� ����������� �������� ��������� ������ � CA
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
        assert(!is_int && "Destroy must be called before destructor!");

        if (is_int)
        {
            destroy();
        }
    }

    // ������������� ����������: ��������� ������ ��� FSA � CA
    void init()
    {
        assert(!is_int && "Allocator already initialized!");

        if (!is_int)
        {
            // ������������� ����� FSA
            for (const auto& [size, count] : blocks_FSA)
            {
                vector<void*>& pool = pools_FSA[size];

                for (size_t i = 0; i < count; ++i)
                {
                    void* block = malloc(size);

                    if (!block) {
                        throw runtime_error("Memory allocation failed for FSA block");
                    }

                    pool.push_back(block);
                }
            }

            // ������������� ������ CA
            void* memory_CA = malloc(memory_for_coalesce);

            if (!memory_CA) {
                throw std::runtime_error("Memory allocation failed for Coalesced Allocator");
            }

            // �������� ��������� � ���� Block_CA � ������� reinterpret_cast
            // reinterpret_cast ��������� ��������� �������������� �������������� �����
            head_of_list = static_cast<Block_CA*>(memory_CA);
            // ��������� ������ ��������� ������
            head_of_list->size = memory_for_coalesce - sizeof(Block_CA);
            head_of_list->is_free = true;
            head_of_list->next = nullptr;
            head_of_list->prev = nullptr;


            is_int = true;
        }
    }

    // ����������� ����������: ������������ ������ FSA, CA � ��
    void destroy()
    {
        assert(is_int && "Allocator not initialized!");

        // ������������ ������ FSA
        for (auto& [size, pool] : pools_FSA)
        {
            for (void* block : pool)
            {
                free(block);
            }
        }

        pools_FSA.clear();

        // ������������ ������ CA
        Block_CA* current = head_of_list;
        while (current)
        {
            Block_CA* next = current->next;
            free(current);
            current = next;
        }
        head_of_list = nullptr;

        // ������������ ������, ���������� � ��
        for (const Block_OS& block_os : pools_OS)
        {
            if (!VirtualFree(block_os.ptr, 0, MEM_RELEASE))
            {
                cerr << "VirtualFree failed with error: " << GetLastError() << "\n";
            }
        }

        pools_OS.clear();


        is_int = false;
    }

    // ��������� ������ ��������� �������
    void* alloc(size_t size)
    {
        assert(is_int && "Allocator not initialized!");

        // ������������ ������� �� ������� 8 ����
        size = (size + 7) & ~7; 

        // ���� ������ ������������� FSA, �������� ����� ���� �� ����
        if (size == 16 || size == 32 || size == 64 || size == 128 || size == 256 || size == 512)
        {
            auto it = pools_FSA.find(size);

            if (it != pools_FSA.end() && !it->second.empty())
            {
                // ����� ��������� ���� �� ����
                void* block = it->second.back(); 
                // ������� ��� �� ������ ���������
                it->second.pop_back();

                return block;
            }
        }
        // ���� ������ �������� ��� CA
        else if (size <= limit_to_call_os)
        {
            Block_CA* current = head_of_list;

            while (current)
            {
                if (current->is_free && current->size >= size)
                {
                    current->is_free = false;

                    // ��������� �����, ���� ������� ���������� �����
                    if (current->size > size + sizeof(Block_CA))
                    {
                        Block_CA* newBlock = reinterpret_cast<Block_CA*>(reinterpret_cast<char*>(current) + sizeof(Block_CA) + size);

                        // ��������� ������ ������ �����
                        newBlock->size = current->size - size - sizeof(Block_CA);
                        newBlock->is_free = true;
                        newBlock->next = current->next;
                        newBlock->prev = current;

                        if (current->next)
                        {
                            current->next->prev = newBlock;
                        }

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
            // ���� ������ ��������� ����� CA, �������� ������ � ��
            void* memory_os = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

            if (!memory_os)
            {
                cerr << "VirtualAlloc failed with error: " << GetLastError() << "\n";

                return nullptr;
            }

            pools_OS.push_back({ memory_os, size });

            return memory_os;
        }

        // ���� ������ �� ������� ��������
        return nullptr;
    }

    // ������������ ������
    void free(void* ptr)
    {
        assert(is_int && "Allocator not initialized!");

        if (!ptr)
        {
            return;
        }

        // ������������ ����� ��� FSA
        for (auto& [size, pool] : pools_FSA)
        {
            for (void* block : pool)
            {
                if (ptr >= block && ptr < reinterpret_cast<void*>(reinterpret_cast<char*>(block) + size))
                {
                    pool.push_back(ptr);

                    return;
                }
            }
        }

        // �������� �������������� � CA
        if (ptr > head_of_list && ptr < reinterpret_cast<void*>(reinterpret_cast<char*>(head_of_list) + memory_for_coalesce))
        {
            Block_CA* block = reinterpret_cast<Block_CA*>(reinterpret_cast<char*>(ptr) - sizeof(Block_CA));
            block->is_free = true;

            merge_free_blocks(block);

            return;
        }

        // �������� �������������� � ������ ��
        for (auto it = pools_OS.begin(); it != pools_OS.end(); ++it)
        {
            if (it->ptr == ptr)
            {
                if (!VirtualFree(it->ptr, 0, MEM_RELEASE))
                {
                    cerr << "VirtualFree failed with error: " << GetLastError() << "\n";
                }

                pools_OS.erase(it);

                return;
            }
        }
    }

//#ifdef DEBUG
    // ������� � ����������� ����� ������ ���������� �� ���������� : ���������� ������� � ��������� ������, ������ ������ ����������� � �� � �� �������
    void dump_stat() const 
    {
        cout << "Memory statistics:\n";

        cout << "Fixed-size Memory Allocation:\n";

        for (const auto& [size, pool] : pools_FSA) 
        {
            size_t block_count = blocks_FSA.at(size);
            size_t free_blocks = pool.size();
            size_t occupied_blocks = block_count - free_blocks;

            cout << "  Block size: " << size << ", Occupied: " << occupied_blocks << ", Free: " << free_blocks << "\n";
        }


        cout << "Coalesce Allocation:\n";

        size_t blocks_free = 0, occupied_blocks = 0;
        Block_CA* current = head_of_list;

        while (current) 
        {
            if (current->is_free) {
                blocks_free++;
            }
            else {
                occupied_blocks++;
            }

            current = current->next;
        }

        cout << "  Occupied: " << occupied_blocks << ", Free: " << blocks_free << "\n";


        cout << "OS Allocations:\n";

        cout << "  Total blocks: " << pools_OS.size() << "\n";
    }

    // ������� � ����������� ����� ������ ������ ���� ���� ������� ������ : �� ������ � �������
    void dump_blocks() const
    {
        cout << "Allocated blocks:\n";


        cout << "Fixed-size Memory Allocation:\n";

        for (const auto& [size, pool] : pools_FSA)
        {
            size_t occupied_blocks = blocks_FSA.at(size) - pool.size();

            cout << "  Block size: " << size << "\n";
            cout << "    Occupied blocks:\n";

            for (size_t i = 0; i < occupied_blocks; ++i)
            {
                void* block = pool[i];

                cout << "      Block at " << block << ", size: " << size << "\n";
            }
        }


        cout << "Coalesce Allocation:\n";

        Block_CA* current = head_of_list;

        while (current)
        {
            if (!current->is_free) {
                cout << "  Block at " << current << ", size: " << current->size << "\n";
            }
            current = current->next;
        }


        cout << "OS Allocations:\n";

        for (const auto& [ptr, size] : pools_OS) {
            cout << "  Block at " << ptr << ", size: " << size << "\n";
        }
    }
//#endif
};
