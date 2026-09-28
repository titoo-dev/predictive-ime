// Identité COM/TSF du text service.
//
// Ces GUID sont l'ADRESSE PERMANENTE du service : Windows les inscrit dans la
// base de registre et dans le profil clavier de l'utilisateur. Les changer sur
// une installation déjà déployée y laisse un profil fantôme que l'utilisateur
// doit retirer à la main — ils ne bougent plus.
#pragma once

// winsock2.h AVANT tout windows.h — os_compat.h s'en charge et doit donc venir
// en premier ici aussi (un windows.h nu tirerait l'ancien winsock.h).
#include "../../core/os_compat.h"

#include <initguid.h>
#include <windows.h>

// {5F0A1C7E-3B84-4D2E-9C31-7A6E2D4B8F10} — le service lui-même (CLSID).
DEFINE_GUID(CLSID_PredictTextService, 0x5f0a1c7e, 0x3b84, 0x4d2e, 0x9c, 0x31,
            0x7a, 0x6e, 0x2d, 0x4b, 0x8f, 0x10);

// {5F0A1C7E-3B84-4D2E-9C31-7A6E2D4B8F11} — le profil (une entrée « Predict »
// dans la liste des claviers).
DEFINE_GUID(GUID_PredictProfile, 0x5f0a1c7e, 0x3b84, 0x4d2e, 0x9c, 0x31, 0x7a,
            0x6e, 0x2d, 0x4b, 0x8f, 0x11);

// {5F0A1C7E-3B84-4D2E-9C31-7A6E2D4B8F12} — attribut d'affichage « saisie en
// cours » (le soulignement du préedit).
DEFINE_GUID(GUID_PredictDisplayAttributeInput, 0x5f0a1c7e, 0x3b84, 0x4d2e, 0x9c,
            0x31, 0x7a, 0x6e, 0x2d, 0x4b, 0x8f, 0x12);

// {5F0A1C7E-3B84-4D2E-9C31-7A6E2D4B8F13} — attribut « texte fantôme »
// (complétion proposée, plus pâle que ce qui est réellement tapé).
DEFINE_GUID(GUID_PredictDisplayAttributeGhost, 0x5f0a1c7e, 0x3b84, 0x4d2e, 0x9c,
            0x31, 0x7a, 0x6e, 0x2d, 0x4b, 0x8f, 0x13);

// {5F0A1C7E-3B84-4D2E-9C31-7A6E2D4B8F14} — compartiment GLOBAL « prédiction
// activée » : partagé par toutes les applications du bureau, c'est lui qui
// fait qu'un clic sur l'indicateur de la barre des tâches coupe la prédiction
// partout, pas seulement dans la fenêtre au premier plan.
DEFINE_GUID(GUID_PredictCompartmentEnabled, 0x5f0a1c7e, 0x3b84, 0x4d2e, 0x9c,
            0x31, 0x7a, 0x6e, 0x2d, 0x4b, 0x8f, 0x14);

#define PREDICT_SERVICE_NAME L"Predict"
#define PREDICT_SERVICE_DESC L"predictive-ime — saisie prédictive FR/EN"
