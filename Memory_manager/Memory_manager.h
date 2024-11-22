#pragma once

#include <iostream>
#include <vector>
#include <map>          // ��� ������������� ���������� map
#include <cassert>      // ��� ������������� ������� assert ��� �������� ������������ ������� � ���������
#include <cstdlib>      // ��� malloc, free � ���������� ����� ����������
#include <windows.h>    // ��� ������� VirtualAlloc � VirtualFree ��� ������ � ������� ��

using namespace std;

// ������������� �������� ������, ������� �������� � ����� ��������� � ���������� �������: Fixed-Size Allocator (FSA),
// Coalesced Allocator (CA) � ���������� ������ � ������������ ������� (OS allocation)
// 
//      FSA ������������ ���������� ������� ��� ��������� ������ �������������� ������� 
//          ��������� � ������������ ������ ����������� �� O(1), ��������� ��� ������ �������� �� ����������
//          ����� �������������� ������� ��������� ���������� ������������
//          ������������� ������������ ������ ������ ������� ������������ ������ ������������� � ��� ���������������� ���������� �� ������� ��������
//
//      CA ��������� ������� ��� ������ ����������� �������, ������� ������ ������ ��� ��������� � �� (limit_to_call_os)
//          ������ ���������� �� ������� ��������������� �������� ����� (memory_for_coalesce), ��� ��������� ����� �������� ��� �������� ����������� ������
//          ��� ������������ �������� ��������� ����� ������������� ������������ ��� ����������� ������������
//          ���� ���������� ������ ����� ���������� �����, ���������� ������ ����������� �� ����� ��������� ����
//          �������� ��� ������������� ������ � ����������� ������������� ��� ��������� �������� �� ������
//
//      OS ��������� ������� ��� ������, ������� ��������� ����� ��� CA
//          ������������ ��������� ������ (VirtualAlloc) ��� ��������� ������� �������� ������
//          ��� ������������ ������ ���������� ��������������� ��������� ����� (VirtualFree)
//          ������ ������ ������ �������������� ��������, ��� ����������� ��� ����������
//          ����� ������ ������� ������ ������� �� ��������� � FSA � CA, ������� ������������ ������ ��� ������� ��������.      
class Memory_allocator
{
private:
    bool is_int = false;

    map<size_t, size_t> blocks_FSA = { {16, 10}, {32, 10}, {64, 10}, {128, 10}, {256, 10}, {512, 10} };
    map<size_t, void*> pools_FSA;

//#ifdef DEBUG
    map<size_t, vector<void*>> occupied_FSA;
//#endif

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
        assert(!is_int && "Destroy must be called before destructor!");

        if (is_int)
        {
            destroy();
        }
    }

    // ������������� ��������� ������:
    // 1. ������������� ����� FSA: ���������� ������ ��� ������������� ������ � ����������� ����������� ������
    // 2. ������������� CA: ���������� ������� ���� ������, ������� ���������� ������� ����������� ������
    void init()
    {
        assert(!is_int && "Allocator already initialized!");

        if (!is_int)
        {
            // ������������� ����� FSA
            for (const auto& [size, count] : blocks_FSA)
            {
                void* head = nullptr; // ������ ������ ��������� ������

                for (size_t i = 0; i < count; ++i)
                {
                    void* block = malloc(size);

                    if (!block)
                    {
                        throw runtime_error("Memory allocation failed for FSA block");
                    }

                    // ��������� ��������� �� ������� "��������" ���� � �����
                    *reinterpret_cast<void**>(block) = head;
                    head = block;
                }

                pools_FSA[size] = head; // ������������� �������� ���� ��� �������� �������
            }

            // ������������� ������ CA
            void* memory_CA = malloc(memory_for_coalesce);

            if (!memory_CA) {
                throw runtime_error("Memory allocation failed for Coalesced Allocator");
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
        for (auto& [size, head] : pools_FSA)
        {
            while (head)
            {
                void* next = *reinterpret_cast<void**>(head);   // �������� ��������� �� ��������� ��������� ����
                free(head);
                head = next;
            }
        }
        pools_FSA.clear();

//#ifdef DEBUG
        // ������������ ������� ������ FSA
        for (auto& [size, blocks] : occupied_FSA)
        {
            for (void* block : blocks)
            {
                if (block) {
                    free(block);
                }
            }
        }
        occupied_FSA.clear();
//#endif

        // ������������ ������ CA
        Block_CA* current = head_of_list;

        while (current)
        {
            Block_CA* next = current->next;

            if (current->is_free)
            {
                free(current);
            }

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

    // �������� ��������� ������:
    // 1. ������� ��������� �� ����� FSA ��� ������������� ��������
    // 2. ���� ������ ������ ������ CA, ������ ���������� ��������� ���� � CA
    //    ���� �� ������, �������� ��������� �� ��� �����
    // 3. ���� ������ ��������� ����� CA, ������ ���������� � �� ����� VirtualAlloc
    void* alloc(size_t size)
    {
        assert(is_int && "Allocator not initialized!");

        // ������������ ������� �� ������� 8 ����
        size = (size + 7) & ~7; 

        // ���� ������ ������������� FSA, �������� ����� ���� �� ����
        // blocks_FSA.count(size) ���������� 1, ���� ���� size ���������� � blocks_FSA, � 0 � ��������� ������.
        if (blocks_FSA.count(size) > 0)
        {
            auto it = pools_FSA.find(size);

            if (it != pools_FSA.end() && it->second)
            {
                void* block = it->second;                           // �������� ����
                it->second = *reinterpret_cast<void**>(block);      // ��������� ��������� ����

//#ifdef DEBUG
                occupied_FSA[size].push_back(block);
//#endif

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

            // nullptr � ����� ������ ����������� ����� ������. ���� �������� nullptr, ������� ���� ������� ���������� �����
            // size � ������ ������ � ������, ������� ����� ��������. ������ ����������� �� �������� ������� �������� (������ 4 ��)
            // MEM_RESERVE � ����������� ����������� ������ ��� ��������� ���������� ������� ������
            // ��� ������ �� �������� �� ��� ���, ���� �� ����� "����������" ����� MEM_COMMIT
            // MEM_COMMIT � �������� ���������� �������� ������ � ��������� �� � ����������� �������
            // PAGE_READWRITE � ������������� ����� ������� �� ������: ������ � ������ ��������
            // ���������� ��������� �� ������ ����������� �������
            void* memory_os = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

            // ���� �������� nullptr, �� ������ �� ����������. ��� ����������� ������������ GetLastError, ������� ���������� ��� ������
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

    // �������� ������������ ������:
    // 1. ���� ��������� ����������� ���� FSA, ���� ������������ � ������ ���������������� ������
    // 2. ���� ��������� ����������� ������ CA, ���� ���������� ���������, � ����� ������������ � ���������
    // 3. ���� ��������� ����������� ������ ��, ������ ������������� ����� VirtualFree
    void free_(void* ptr)
    {
        assert(is_int && "Allocator not initialized!");

        if (!ptr)
        {
            return;
        }

//#ifdef DEBUG
        // ������������ ����� ��� FSA
        for (auto& [size, occupied] : occupied_FSA) 
        {
            auto it = find(occupied.begin(), occupied.end(), ptr);

            if (it != occupied.end())
            {
                // erase �  ������ �������� ��������� �� �������
                occupied.erase(it);

            }
        }
//#endif

        // ������������ ����� ��� FSA
        for (auto& [size, head] : pools_FSA) 
        {
            // ���������� ���������� ������ ����� �� ��������� ����������� ��������� � ������� ��� � ���
            if (reinterpret_cast<void**>(ptr) == &head) 
            {
                // ���������� ���� � ������ ������ ��������� ������
                *reinterpret_cast<void**>(ptr) = head;  // ��������� �� ���������� ������ ������
                head = ptr;                             // ����� ������� ������ ���������� �������������� ����

                return;
            }
        }

        // �������� �������������� � CA
        if (ptr > head_of_list && ptr < reinterpret_cast<void*>(reinterpret_cast<char*>(head_of_list) + memory_for_coalesce))
        {
            Block_CA* block = reinterpret_cast<Block_CA*>(reinterpret_cast<char*>(ptr) - sizeof(Block_CA));     // �������� �������������� �����

            if (!block->is_free)
            {
                block->is_free = true;
                merge_free_blocks(block);
            }

            return;
        }

        // �������� �������������� � ������ ��
        for (auto it = pools_OS.begin(); it != pools_OS.end(); ++it)
        {
            if (it->ptr == ptr)
            {
                // 0 � ������ �������������� �������. ���� �������� 0, ������������� ��� �������, ���������� ����� VirtualAlloc (��� ������������� MEM_RELEASE)
                // MEM_RELEASE � ���������, ��� �� ����������� ���������� ������. ������ ������ �� �������� ��������
                // ���� ������� ���������� FALSE, ��������� ������. 
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
    // ����� ���������� ������� � ��������� ������ ��� FSA, CA � ��
    void dump_stat() const
    {
        cout << "\nMemory statistics:\n****************************************\n\n";

        cout << "Fixed-size Memory Allocation:\n----------------------------------------\n";

        for (const auto& [size, head] : pools_FSA)
        {
            size_t free_blocks = 0;

            // ������� ��������� ������ � ������
            void* current = head;

            while (current)
            {
                ++free_blocks;
                current = *reinterpret_cast<void**>(current); // ������� � ���������� �����
            }

            // ��������� ������� ����� � occupied_FSA
            size_t occupied_blocks = 0;
            auto it = occupied_FSA.find(size);

            if (it != occupied_FSA.end())
            {
                occupied_blocks = it->second.size(); // ���������� ������� ������
            }

            cout << "Size: " << size << ", Occupied: " << occupied_blocks << ", Free: " << free_blocks << "\n";
        }

        cout << "----------------------------------------\n\nCoalesce Allocation:\n----------------------------------------\n";

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

        cout << "Occupied: " << occupied_blocks << ", Free: " << blocks_free << "\n";

        cout << "----------------------------------------\n\nOS Allocations:\n----------------------------------------\n";
        cout << "Occupied blocks: " << pools_OS.size() << "\n";
        cout << "----------------------------------------\n";

        cout << "\n****************************************\n";
    }

    // ����� ������ � ������� ���� ���������� ������ ��� FSA, CA � ��
    void dump_blocks() const
    {
        cout << "\nAllocated blocks:\n****************************************\n\n";


        cout << "Fixed-size Memory Allocation:\n----------------------------------------\n";

        for (const auto& [size, head] : pools_FSA)
        {
            // ��������� ������� ����� � occupied_FSA
            auto it = occupied_FSA.find(size);

            if (it != occupied_FSA.end())
            {
                const vector<void*>& occupied_blocks = it->second;

                if (occupied_blocks.size() > 0)
                {
                    for (void* block : occupied_blocks)
                    {
                        cout << "Block at " << block << ", size: " << size << "\n";
                    }
                }
            }
        }

        cout << "----------------------------------------\n\nCoalesce Allocation:\n----------------------------------------\n";

        Block_CA* current = head_of_list;

        while (current)
        {
            if (!current->is_free) {
                cout << "Block at " << current << ", size: " << current->size << "\n";
            }
            current = current->next;
        }


        cout << "----------------------------------------\n\nOS Allocations:\n----------------------------------------\n";

        for (const auto& [ptr, size] : pools_OS) {
            cout << "  Block at " << ptr << ", size: " << size << "\n";
        }

        cout << "----------------------------------------\n";


        cout << "\n****************************************\n";
    }
//#endif
};