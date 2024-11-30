#pragma once

//#ifdef DEBUG
#include <iostream>
//#endif

#include <cassert>  
#include <array>
#include <windows.h>    // Для вызовов VirtualAlloc и VirtualFree при работе с памятью ОС

using namespace std;

// Универсальный менеджер памяти, работающий с тремя подходами к управлению памятью: Fixed-Size Allocator (FSA), 
// Coalesced Allocator (CA) и выделением памяти у операционной системы (OS allocation).
//
// Fixed-Size Allocator (FSA):
// - Операции выделения и освобождения памяти выполняются за O(1) благодаря использованию односвязного списка.
// - Исключает внутреннюю фрагментацию за счет фиксированного размера блоков.
// - Порядок освобождения блоков не влияет на производительность — блоки переиспользуются независимо.
//
// Coalesced Allocator (CA):
// - Управляет памятью для блоков переменного размера, которые меньше заданного предела (limit_to_call_os).
// - Использует большой заранее выделенный блок памяти (memory_for_coalesce).
// - Свободные блоки хранятся в двусвязном списке, что упрощает управление и объединение.
// - При освобождении соседние свободные блоки автоматически объединяются, уменьшая фрагментацию.
// - Если выделяемый размер меньше свободного блока, оставшаяся память становится новым свободным блоком.
//
// OS Allocation:
// - Обрабатывает запросы памяти для больших блоков, превышающих заданный лимит (limit_to_call_os).
// - Каждый запрос памяти обрабатывается индивидуально, без объединения или разделения блоков.
// - Такие вызовы более затратны по времени, поэтому применяются только для крупных запросов.    
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

        // Конструктор для удобной инициализации
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

    // Метод объединения соседних свободных блоков в CA
    // Если предыдущий или следующий блоки свободны, они объединяются с текущим для минимизации фрагментации
    void merge_free_blocks(Block_CA* block)
    {
        // Если предыдущий блок свободен, объединяем его с текущим
        if (block->prev && block->prev->is_free)
        {
            // Увеличиваем размер предыдущего блока
            block->prev->size += block->size + sizeof(Block_CA); 
            // Пропускаем текущий блок в списке
            block->prev->next = block->next; 

            if (block->next)
            {
                // Обновляем указатель на предыдущий у следующего блока
                block->next->prev = block->prev; 
            }

            // Смещаем текущий блок на предыдущий
            block = block->prev;
        }

        // Если следующий блок свободен, объединяем его с текущим (логика аналогичная)
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

    // Инициализирует память для FSA и CA, выделяя общий блок
    void init()
    {
        assert(!is_init && "Allocator already initialized!");

        if (is_init) {
            return;
        }

        // Рассчитываем общий объем памяти, необходимый для FSA и CA
        size_t memory_FSA = 0;

        for (const auto& pool : pools_FSA) {
            memory_FSA += (sizeof(FSA_Pool) + (pool.size + sizeof(Block_FSA)) * pool.count);
        }

        // nullptr — адрес начала выделяемого блока памяти. Если передать nullptr, система сама выберет подходящий адрес
        // total_size — размер памяти в байтах, который нужно выделить. Размер округляется до кратного размера страницы (обычно 4 КБ)
        // MEM_RESERVE — резервирует виртуальную память без выделения физических страниц памяти
        // Эта память не доступна до тех пор, пока не будет "закреплена" через MEM_COMMIT
        // MEM_COMMIT — выделяет физические страницы памяти и связывает их с виртуальной памятью
        // PAGE_READWRITE — устанавливает права доступа на память: чтение и запись разрешен
        // Возвращает указатель на начало выделенного региона
        ptr_main = VirtualAlloc(nullptr, memory_FSA + memory_CA, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

        // Разделяем память на части: FSA и CA
        // uint8_t — это тип беззнакового целого числа размером 1 байт (8 бит), который идеально подходит для работы с адресами и смещениями в памяти
        // Использование uint8_t* позволяет выполнять арифметические операции с указателями на уровне отдельных байтов
        uint8_t* current_ptr = reinterpret_cast<uint8_t*>(ptr_main);

        // Инициализация FSA
        for (auto& pool : pools_FSA)
        {
            pool.head = current_ptr;

            // Разделяем память внутри пула на блоки
            // Последний блок указывает на nullptr
            Block_FSA* block = static_cast<Block_FSA*>(pool.head);

            for (size_t i = 0; i < pool.count - 1; ++i)
            {
                // Приводим указатель к типу Block_FSA с помощью reinterpret_cast
                // reinterpret_cast позволяет выполнять низкоуровневое преобразование типов
                block->next = reinterpret_cast<Block_FSA*>(reinterpret_cast<uint8_t*>(block) + pool.size + sizeof(Block_FSA));
                block = block->next;
            }

            current_ptr += (pool.size + sizeof(Block_FSA)) * pool.count;
        }

        // Инициализация CA
        head_CA = reinterpret_cast<Block_CA*>(current_ptr);
        // Вычисляем размер доступной памяти
        head_CA->size = memory_CA - sizeof(Block_CA);
        head_CA->is_free = true;
        head_CA->next = nullptr;
        head_CA->prev = nullptr;

        is_init = true;
    }

    // Уничтожает аллокатор, освобождая всю выделенную память
    void destroy()
    {
        assert(is_init && "Allocator not initialized!");

        // ptr_main - указатель на начало региона памяти, который нужно освободить
        // MEM_RELEASE - указывает, что нужно освободить весь регион памяти (поэтому 2 параметр игнорируется, можно отправить даже 0)
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

    // Выделяет блок памяти заданного размера
    // FSA: возвращает блок из пула фиксированного размера
    // CA: возвращает блок из динамической памяти
    // OS: выделяет блок через VirtualAlloc
    void* alloc(size_t size)
    {
        assert(is_init && "Allocator not initialized!");

        // Выравнивание размера на границу 8 байт
        size = (size + 7) & ~7; 

        // Если размер соответствует FSA, пытаемся взять блок из пула
        if (size <= pools_FSA.back().size) 
        {
            for (auto& pool : pools_FSA) 
            {
                if (size <= pool.size) 
                {
                    if (pool.head) 
                    {
                        Block_FSA* block = static_cast<Block_FSA*>(pool.head);
                        pool.head = block->next;  // Перемещаем голову на следующий блок

                        // block + 1 используется, чтобы получить указатель на данные, которые находятся после метаданных блока
                        return reinterpret_cast<void*>(block + 1);
                    }
                }
            }
        }
        // Если размер подходит для CA
        else if (size <= limit_to_call_os)
        {
            Block_CA* current = head_CA;

            while (current)
            {
                if (current->is_free && current->size >= size)
                {
                    current->is_free = false;

                    // Разбиение блока, если остаток достаточно велик
                    if (current->size > size + sizeof(Block_CA))
                    {
                        // Приводим указатель к типу Block_CA с помощью reinterpret_cast
                        // reinterpret_cast позволяет выполнять низкоуровневое преобразование типов
                        Block_CA* new_block = reinterpret_cast<Block_CA*>(reinterpret_cast<char*>(current) + sizeof(Block_CA) + size);

                        // Вычисляем размер нового блока
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

                    // current + 1 используется, чтобы получить указатель на данные, которые находятся после метаданных блока
                    return reinterpret_cast<void*>(current + 1);
                }

                current = current->next;
            }
        }
        else
        {
            // Если размер превышает лимит CA, выделяем память у ОС
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

        // Если память не удалось выделить
        return nullptr;
    }
 
    // Освобождает блок памяти:
    // FSA: возвращает блок в пул
    // CA: помечает блок как свободный и объединяет его с соседними
    // OS: освобождает память через VirtualFree
    void free_(void* ptr)
    {
        assert(is_init && "Allocator not initialized!");

        if (!ptr)
        {
            return;
        }

        for (auto& pool : pools_FSA) 
        {
            // Проверка принадлежности к FSA
            if (ptr >= pool.head && ptr < reinterpret_cast<void*>(reinterpret_cast<char*>(pool.head) + pool.count * (pool.size + sizeof(Block_FSA)))) 
            {
                Block_FSA* block = static_cast<Block_FSA*>(ptr);
                block->next = static_cast<Block_FSA*>(pool.head);
                pool.head = block;

                return;
            }
        }

        // Проверка принадлежности к CA
        if (ptr > head_CA && ptr < reinterpret_cast<void*>(reinterpret_cast<char*>(head_CA) + memory_CA))
        {
            Block_CA* block = reinterpret_cast<Block_CA*>(reinterpret_cast<char*>(ptr) - sizeof(Block_CA));     // Получаем метаинформацию блока

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
            // Проверка принадлежности к OS
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
    // Вывод количества занятых и свободных блоков для CA и ОС
    // Для FSA надо заводить списки занятых для каждого размера, но мне лень уже
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

    // Вывод адреса и размеры всех выделенных блоков для FSA, CA и OS
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