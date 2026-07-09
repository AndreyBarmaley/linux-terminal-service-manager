#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <algorithm>

// Подключение библиотеки для загрузки изображений
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Структуры согласно техническому заданию
struct Square {
    uint16_t x;
    uint16_t y;
    uint16_t size;
    uint16_t padding; // Выравнивание структуры до 8 байт для производительности
};

struct Color {
    uint32_t rgba; // Упакованный цвет в формате 0xRRGGBBAA
};

struct RenderQuad {
    Square rect;
    Color color;
};

// Функция проверки абсолютной однородности региона (Lossless)
bool check_homogeneity(const uint8_t* img, int img_w, int x, int y, int size, uint32_t& out_color) {
    // Берем цвет самого первого (верхнего левого) пикселя за эталон
    int first_idx = (y * img_w + x) * 4;
    uint8_t r_first = img[first_idx];
    uint8_t g_first = img[first_idx + 1];
    uint8_t b_first = img[first_idx + 2];
    uint8_t a_first = img[first_idx + 3];

    // Проверяем, совпадают ли все остальные пиксели в квадрате с эталоном
    for (int row = 0; row < size; ++row) {
        for (int col = 0; col < size; ++col) {
            int idx = ((y + row) * img_w + (x + col)) * 4;
            if (img[idx]     != r_first || 
                img[idx + 1] != g_first || 
                img[idx + 2] != b_first || 
                img[idx + 3] != a_first) {
                return false; // Найдено несовпадение, нужно дробить дальше
            }
        }
    }

    // Если все пиксели строго одинаковы, упаковываем цвет в uint32_t
    out_color = (static_cast<uint32_t>(r_first) << 24) | 
                (static_cast<uint32_t>(g_first) << 16) | 
                (static_cast<uint32_t>(b_first) << 8)  | 
                a_first;
    return true;
}

// Рекурсивный обход квадродерева (Quadtree)
void process_quad(const uint8_t* img, int img_w, int x, int y, int size, std::vector<RenderQuad>& out_list) {
    uint32_t exact_color = 0;
    
    // Базовый случай: размер 1x1 или регион абсолютно однороден
    if (size == 1 || check_homogeneity(img, img_w, x, y, size, exact_color)) {
        if (size == 1) {
            // Если дошли до 1 пикселя, просто считываем его цвет
            int idx = (y * img_w + x) * 4;
            exact_color = (static_cast<uint32_t>(img[idx]) << 24) | 
                          (static_cast<uint32_t>(img[idx + 1]) << 16) | 
                          (static_cast<uint32_t>(img[idx + 2]) << 8)  | 
                          img[idx + 3];
        }
        
        RenderQuad quad;
        quad.rect = { static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(size), 0 };
        quad.color = { exact_color };
        out_list.push_back(quad);
        return;
    }

    // Рекурсивное деление на 4 подквадрата (длина стороны строго делится на 2)
    int half = size / 2;
    process_quad(img, img_w, x, y, half, out_list);               // Топ-Лево
    process_quad(img, img_w, x + half, y, half, out_list);        // Топ-Право
    process_quad(img, img_w, x, y + half, half, out_list);        // Бот-Лево
    process_quad(img, img_w, x + half, y + half, half, out_list); // Бот-Право
}

// Главная функция для запуска алгоритма разбиения
std::vector<RenderQuad> image_to_quad_sequence(const std::string& filename) {
    int width, height, channels;
    // Принудительно загружаем изображение в режиме RGBA (4 канала)
    uint8_t* img_data = stbi_load(filename.c_str(), &width, &height, &channels, 4);
    
    std::vector<RenderQuad> result;
    if (!img_data) {
        std::cerr << "Не удалось загрузить файл: " << filename << std::endl;
        return result;
    }

    // Для корректной работы алгоритма Квадродерева требуется квадратная область,
    // размер которой равен степени двойки (2, 4, 8, 16 ... 1024, 2048).
    // Берем минимальную сторону, чтобы гарантировать отсутствие выхода за границы текстуры.
    int min_side = std::min(width, height);
    int pow2_side = 1 << static_cast<int>(std::log2(min_side)); 

    // Оптимизация: резервируем память в векторе во избежание частых перевыделений
    result.reserve((pow2_side * pow2_side) / 4);

    // Запуск рекурсивного анализа с нулевых координат на всю рабочую область
    process_quad(img_data, width, 0, 0, pow2_side, result);

    // Освобождаем память, выделенную библиотекой stb_image
    stbi_image_free(img_data);
    return result;
}
