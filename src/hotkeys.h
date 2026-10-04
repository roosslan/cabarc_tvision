#pragma once

// Поддержка горячих клавиш с кириллическими буквами (~Ф~айл, ~Д~а).
// Turbo Vision сравнивает выделенный символ побайтно и не распознаёт
// буквы в UTF-8. Буква определяется по нажатой клавише: Alt+клавиша
// соответствует букве на той же клавише раскладки ЙЦУКЕН, либо берётся
// введённый кириллический символ.

#define Uses_TEvent
#define Uses_TView
#define Uses_TMenuBar
#define Uses_TSubMenu
#include <tvision/tv.h>

// Строка меню, открывающая подменю по Alt+русская буква.
class TCabMenuBar : public TMenuBar
{
public:
    TCabMenuBar(const TRect &bounds, TSubMenu &aMenu);
    void handleEvent(TEvent &event) override;
};

// Обработка нажатия до его маршрутизации. Вызывается из TApplication::getEvent.
// 1. Символу вне кодовой страницы (кириллица) назначается ненулевой код
//    клавиши: иначе TLabel, TButton и TCluster принимают его за свою горячую
//    клавишу (getAltCode() кириллической буквы, как и kbNoKey, равен нулю)
//    и перехватывают ввод русского текста.
// 2. Кириллическая горячая буква превращается в действие над активным
//    модальным видом (открытое меню или диалог).
void translateCyrillicHotkey(TEvent &event, TView *topView);
