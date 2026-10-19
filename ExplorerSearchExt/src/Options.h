#pragma once
#include <atomic>

// Option partagee : rechercher le nom complet (avec extension) a l'identique,
// ou seulement une sous-chaine. Lue au moment de la requete.
struct SearchOptions
{
    std::atomic<bool> matchFullName{ false }; // false = sous-chaine, true = nom complet exact
};
SearchOptions& Options();

extern HINSTANCE g_hinst;
