// Durée de vie de la DLL elle-même.
//
// Le text service est une DLL COM in-process : l'application hôte (explorer,
// SearchHost, Word…) appelle régulièrement CoFreeUnusedLibraries, qui DÉCHARGE
// toute DLL dont DllCanUnloadNow répond S_OK. Si elle répond S_OK alors qu'un
// objet, une fenêtre, un crochet ou un thread de la DLL existe encore, le
// prochain rappel saute dans de la mémoire démappée : c'est le
// « predict-tsf.dll_unloaded » du journal d'événements, et l'explorateur
// tombe « à des moments aléatoires ».
//
// Règle : TOUT ce qui peut être rappelé par l'extérieur tient une référence
// sur le module tant qu'il vit — objets COM (dllAddRef/dllRelease), classes
// de fenêtre (registerWindowClass) et threads (runDetached).
#pragma once

#include <functional>
#include <windows.h>

namespace win {

// HINSTANCE de la DLL — pas celui de l'exécutable hôte. Les classes de
// fenêtre en dépendent : une classe inscrite sous le HINSTANCE de l'hôte
// survit au déchargement de la DLL, avec un WndProc qui pointe dans le vide.
HINSTANCE dllInstance();

// Compteur consulté par DllCanUnloadNow.
void dllAddRef();
void dllRelease();
bool dllHasRefs();

// Inscrit une classe de fenêtre sous le HINSTANCE de la DLL et la retient
// pour la désinscrire au déchargement (Windows ne le fait pas pour une DLL).
// Tolère une classe déjà inscrite (rechargement au même emplacement).
bool registerWindowClass(WNDCLASSEXW &wc);
// À appeler depuis DllMain(DLL_PROCESS_DETACH) hors fin de process.
void unregisterWindowClasses();

// Lance `fn` sur un thread détaché qui tient une VRAIE référence du chargeur
// sur la DLL (GetModuleHandleEx) jusqu'à sa fin : même si COM décide de
// décharger le module entre-temps, le code du thread reste mappé.
bool runDetached(std::function<void()> fn);

} // namespace win
