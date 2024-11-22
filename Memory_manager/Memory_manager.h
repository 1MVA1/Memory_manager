#pragma once

#include <iostream>
#include <vector>
#include <map>          // Для использования контейнера map
#include <cassert>      // Для использования макроса assert для проверки корректности вызовов и состояния
#include <cstdlib>      // Для malloc, free и приведения типов указателей
#include <windows.h>    // Для вызовов VirtualAlloc и VirtualFree при работе с памятью ОС

using namespace std;

// Универсальный менеджер памяти, который работает с тремя подходами к управлению памятью: Fixed-Size Allocator (FSA),
// Coalesced Allocator (CA) и выделением памяти у операционной системы (OS allocation)
// 
//      FSA оптимизирует управление памятью для небольших блоков фиксированного размера 
//          Выделение и освобождение блоков выполняются за O(1), поскольку это просто операции на указателях
//          Блоки фиксированного размера устраняют внутреннюю фрагментацию
//          Использование односвязного списка делает порядок освобождения блоков нерелевантным — они переиспользуются независимо от порядка возврата
//
//      CA управляет памятью для блоков переменного размера, которые меньше лимита для обращения к ОС (limit_to_call_os)
//          Память выделяется из заранее подготовленного большого блока (memory_for_coalesce), где свободные блоки хранятся как элементы двусвязного списка
//          При освобождении соседние свободные блоки автоматически объединяются для минимизации фрагментации
//          Если выделяется только часть свободного блока, оставшаяся память разрезается на новый свободный блок
//          Подходит для распределения памяти с минимальной фрагментацией при невысоких запросах на размер
//
//      OS управляет памятью для блоков, которые превышают лимит для CA
//          Используются системные вызовы (VirtualAlloc) для выделения больших областей памяти
//          Для освобождения памяти вызывается соответствующий системный вызов (VirtualFree)
//          Каждый запрос памяти обрабатывается отдельно, без объединения или разделения
//          Такие вызовы требуют больше времени по сравнению с FSA и CA, поэтому используются только для крупных запросов.      
class Memory_allocator
{
private:
    bool is_int = false;

    map<size_t, size_t> blocks_FSA = { {16, 10}, {32, 10}, {64, 10}, {128, 10}, {256, 10}, {512, 10} };
    map<size_t, void*> pools_FSA;

//#ifdef DEBUG
    map<size_t, vector<void*>> occupied_FSA;
//#endif

    // Структура описания блока для CA
    struct Block_CA
    {
        bool is_free;      
        size_t size;   
        Block_CA* next;  
        Block_CA* prev; 
    };

    Block_CA* head_of_list = nullptr; 
    size_t memory_for_coalesce = 4096; 

    // Структура для описания блока, выделенного у ОС
    struct Block_OS
    {
        void* ptr;         
        size_t size;        
    };

    size_t limit_to_call_os = 10 * 1024 * 1024;
    vector<Block_OS> pools_OS;

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
        assert(!is_int && "Destroy must be called before destructor!");

        if (is_int)
        {
            destroy();
        }
    }

    // Инициализация менеджера памяти:
    // 1. Инициализация пулов FSA: выделяется память для фиксированных блоков и формируется односвязный список
    // 2. Инициализация CA: выделяется большой блок памяти, который становится головой двусвязного списка
    void init()
    {
        assert(!is_int && "Allocator already initialized!");

        if (!is_int)
        {
            // Инициализация пулов FSA
            for (const auto& [size, count] : blocks_FSA)
            {
                void* head = nullptr; // Начало списка свободных блоков

                for (size_t i = 0; i < count; ++i)
                {
                    void* block = malloc(size);

                    if (!block)
                    {
                        throw runtime_error("Memory allocation failed for FSA block");
                    }

                    // Сохраняем указатель на текущий "головной" блок в новом
                    *reinterpret_cast<void**>(block) = head;
                    head = block;
                }

                pools_FSA[size] = head; // Устанавливаем головной блок для текущего размера
            }

            // Инициализация списка CA
            void* memory_CA = malloc(memory_for_coalesce);

            if (!memory_CA) {
                throw runtime_error("Memory allocation failed for Coalesced Allocator");
            }

            // Приводим указатель к типу Block_CA с помощью reinterpret_cast
            // reinterpret_cast позволяет выполнять низкоуровневое преобразование типов
            head_of_list = static_cast<Block_CA*>(memory_CA);
            // Вычисляем размер доступной памяти
            head_of_list->size = memory_for_coalesce - sizeof(Block_CA);
            head_of_list->is_free = true;
            head_of_list->next = nullptr;
            head_of_list->prev = nullptr;


            is_int = true;
        }
    }

    // Уничтожение аллокатора: освобождение памяти FSA, CA и ОС
    void destroy()
    {
        assert(is_int && "Allocator not initialized!");

        // Освобождение памяти FSA
        for (auto& [size, head] : pools_FSA)
        {
            while (head)
            {
                void* next = *reinterpret_cast<void**>(head);   // Получаем указатель на следующий свободный блок
                free(head);
                head = next;
            }
        }
        pools_FSA.clear();

//#ifdef DEBUG
        // Освобождение занятых блоков FSA
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

        // Освобождение памяти CA
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

        // Освобождение памяти, выделенной у ОС
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

    // Алгоритм выделения памяти:
    // 1. Попытка выделения из пулов FSA для фиксированных размеров
    // 2. Если размер меньше лимита CA, ищется подходящий свободный блок в CA
    //    Если он найден, возможно разбиение на два блока
    // 3. Если запрос превышает лимит CA, память выделяется у ОС через VirtualAlloc
    void* alloc(size_t size)
    {
        assert(is_int && "Allocator not initialized!");

        // Выравнивание размера на границу 8 байт
        size = (size + 7) & ~7; 

        // Если размер соответствует FSA, пытаемся взять блок из пула
        // blocks_FSA.count(size) возвращает 1, если ключ size существует в blocks_FSA, и 0 в противном случае.
        if (blocks_FSA.count(size) > 0)
        {
            auto it = pools_FSA.find(size);

            if (it != pools_FSA.end() && it->second)
            {
                void* block = it->second;                           // Головной блок
                it->second = *reinterpret_cast<void**>(block);      // Следующий свободный блок

//#ifdef DEBUG
                occupied_FSA[size].push_back(block);
//#endif

                return block;
            }
        }
        // Если размер подходит для CA
        else if (size <= limit_to_call_os)
        {
            Block_CA* current = head_of_list;

            while (current)
            {
                if (current->is_free && current->size >= size)
                {
                    current->is_free = false;

                    // Разбиение блока, если остаток достаточно велик
                    if (current->size > size + sizeof(Block_CA))
                    {
                        Block_CA* newBlock = reinterpret_cast<Block_CA*>(reinterpret_cast<char*>(current) + sizeof(Block_CA) + size);

                        // Вычисляем размер нового блока
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
            // Если размер превышает лимит CA, выделяем память у ОС

            // nullptr — адрес начала выделяемого блока памяти. Если передать nullptr, система сама выберет подходящий адрес
            // size — размер памяти в байтах, который нужно выделить. Размер округляется до кратного размера страницы (обычно 4 КБ)
            // MEM_RESERVE — резервирует виртуальную память без выделения физических страниц памяти
            // Эта память не доступна до тех пор, пока не будет "закреплена" через MEM_COMMIT
            // MEM_COMMIT — выделяет физические страницы памяти и связывает их с виртуальной памятью
            // PAGE_READWRITE — устанавливает права доступа на память: чтение и запись разрешен
            // Возвращает указатель на начало выделенного региона
            void* memory_os = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

            // Если вернулся nullptr, то память не выделилась. Для диагностики используется GetLastError, которая возвращает код ошибки
            if (!memory_os)
            {
                cerr << "VirtualAlloc failed with error: " << GetLastError() << "\n";

                return nullptr;
            }

            pools_OS.push_back({ memory_os, size });

            return memory_os;
        }

        // Если память не удалось выделить
        return nullptr;
    }

    // Алгоритм освобождения памяти:
    // 1. Если указатель принадлежит пулу FSA, блок возвращается в начало соответствующего списка
    // 2. Если указатель принадлежит памяти CA, блок помечается свободным, а затем объединяется с соседними
    // 3. Если указатель принадлежит памяти ОС, память освобождается через VirtualFree
    void free_(void* ptr)
    {
        assert(is_int && "Allocator not initialized!");

        if (!ptr)
        {
            return;
        }

//#ifdef DEBUG
        // Освобождение блока для FSA
        for (auto& [size, occupied] : occupied_FSA) 
        {
            auto it = find(occupied.begin(), occupied.end(), ptr);

            if (it != occupied.end())
            {
                // erase —  способ удаления элементов из вектора
                occupied.erase(it);

            }
        }
//#endif

        // Освобождение блока для FSA
        for (auto& [size, head] : pools_FSA) 
        {
            // Попытаемся определить размер блока на основании переданного указателя и вернуть его в пул
            if (reinterpret_cast<void**>(ptr) == &head) 
            {
                // Возвращаем блок в начало списка свободных блоков
                *reinterpret_cast<void**>(ptr) = head;  // Указатель на предыдущую голову списка
                head = ptr;                             // Новый головой списка становится освободившийся блок

                return;
            }
        }

        // Проверка принадлежности к CA
        if (ptr > head_of_list && ptr < reinterpret_cast<void*>(reinterpret_cast<char*>(head_of_list) + memory_for_coalesce))
        {
            Block_CA* block = reinterpret_cast<Block_CA*>(reinterpret_cast<char*>(ptr) - sizeof(Block_CA));     // Получаем метаинформацию блока

            if (!block->is_free)
            {
                block->is_free = true;
                merge_free_blocks(block);
            }

            return;
        }

        // Проверка принадлежности к памяти ОС
        for (auto it = pools_OS.begin(); it != pools_OS.end(); ++it)
        {
            if (it->ptr == ptr)
            {
                // 0 — размер освобождаемого региона. Если передать 0, освобождается вся область, выделенная через VirtualAlloc (при использовании MEM_RELEASE)
                // MEM_RELEASE — указывает, что мы освобождаем выделенную память. Память больше не доступна процессу
                // Если функция возвращает FALSE, произошла ошибка. 
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
    // Вывод количества занятых и свободных блоков для FSA, CA и ОС
    void dump_stat() const
    {
        cout << "\nMemory statistics:\n****************************************\n\n";

        cout << "Fixed-size Memory Allocation:\n----------------------------------------\n";

        for (const auto& [size, head] : pools_FSA)
        {
            size_t free_blocks = 0;

            // Подсчет свободных блоков в списке
            void* current = head;

            while (current)
            {
                ++free_blocks;
                current = *reinterpret_cast<void**>(current); // Переход к следующему блоку
            }

            // Проверяем наличие ключа в occupied_FSA
            size_t occupied_blocks = 0;
            auto it = occupied_FSA.find(size);

            if (it != occupied_FSA.end())
            {
                occupied_blocks = it->second.size(); // Количество занятых блоков
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

    // Вывод адреса и размеры всех выделенных блоков для FSA, CA и ОС
    void dump_blocks() const
    {
        cout << "\nAllocated blocks:\n****************************************\n\n";


        cout << "Fixed-size Memory Allocation:\n----------------------------------------\n";

        for (const auto& [size, head] : pools_FSA)
        {
            // Проверяем наличие ключа в occupied_FSA
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