/// @file Survey.h
/// @brief Portable survey engine (Observer domain). Qualcomm adapters are separate.
///
///   SurveyDomain       — Tower / Operator / SurveyStats
///   SurveyProjection   — tracker snapshot → domain (honest RF vs FULL)
///   ModemControl       — IModemControl + caps + intents (Null/Android ports)
///   SurveyStrategy     — PassiveMonitor / LteWalk
///   SurveySession      — task-oriented facade
///
/// Linux SIMCOM lock dialect: <qcom/linux/SimcomAtControl.h>
#pragma once

#include <observer/engine/ModemControl.h>
#include <observer/engine/SurveyDomain.h>
#include <observer/engine/SurveyProjection.h>
#include <observer/engine/SurveySession.h>
#include <observer/engine/SurveyStrategy.h>
