// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "pch.h"
#include "user_statistics_internal.h"
#include "xbox_live_context_internal.h"
#include "real_time_activity_manager.h"

NAMESPACE_MICROSOFT_XBOX_SERVICES_USERSTATISTICS_CPP_BEGIN

UserStatisticsService::UserStatisticsService(
    _In_ User&& user,
    _In_ std::shared_ptr<xbox::services::XboxLiveContextSettings> xboxLiveContextSettings,
    _In_ std::shared_ptr<xbox::services::real_time_activity::RealTimeActivityManager> rtaManager
) noexcept :
    m_user{ std::move(user) },
    m_xboxLiveContextSettings{ std::move(xboxLiveContextSettings) },
    m_rtaManager{ std::move(rtaManager) }
{
}

UserStatisticsService::~UserStatisticsService() noexcept
{
    if (!m_statisticChangeHandlers.empty())
    {
        for (auto& userPair : m_trackedStats)
        {
            for (auto& statPair : userPair.second)
            {
                m_rtaManager->RemoveSubscription(m_user, statPair.second.subscription);
            }
        }
    }
}

HRESULT UserStatisticsService::GetSingleUserStatistic(
    _In_ uint64_t xuid,
    _In_ const String& scid,
    _In_ const String& statisticName,
    _In_ AsyncContext<Result<UserStatisticsResult>> async
) const noexcept
{
    return GetSingleUserStatistics(xuid, scid, Vector<String>{ statisticName }, std::move(async));
}

HRESULT UserStatisticsService::GetSingleUserStatistics(
    _In_ uint64_t xuid,
    _In_ const String& scid,
    _In_ const Vector<String>& statisticNames,
    _In_ AsyncContext<Result<UserStatisticsResult>> async
) const noexcept
{
    RETURN_HR_INVALIDARGUMENT_IF(scid.empty());

    Result<User> userResult = m_user.Copy();
    RETURN_HR_IF_FAILED(userResult.Hresult());

    auto httpCall = MakeShared<XblHttpCall>(userResult.ExtractPayload());
    HRESULT hr = httpCall->Init(
        m_xboxLiveContextSettings,
        "GET",
        XblHttpCall::BuildUrl("userstats", UserStatsSubpath(xuid, scid, statisticNames)),
        xbox_live_api::get_single_user_statistics
    );

    RETURN_HR_IF_FAILED(hr);
    RETURN_HR_IF_FAILED(httpCall->SetXblServiceContractVersion(1));

    return httpCall->Perform(AsyncContext<HttpResult>{
        async.Queue().DeriveWorkerQueue(),
            [
                async,
                scid
            ]
        (HttpResult httpResult)
        {
            HRESULT hr{ Failed(httpResult) ? httpResult.Hresult() : httpResult.Payload()->Result() };
            if (SUCCEEDED(hr))
            {
                auto result = UserStatisticsResult::Deserialize(httpResult.Payload()->GetResponseBodyJson());
                result.Payload().SetServiceConfigurationId(scid);
                return async.Complete(result);
            }
            async.Complete(hr);
        }});
}

HRESULT UserStatisticsService::GetMultipleUserStatistics(
    _In_ const Vector<uint64_t>& xuids,
    _In_ const String& serviceConfigurationId,
    _In_ const Vector<String>& statisticNames,
    _In_ AsyncContext<Result<Vector<UserStatisticsResult>>> async
) const noexcept
{
    return GetMultipleUserStatisticsForMultipleServiceConfigurations(
        xuids,
        Vector<RequestedStatistics>{ RequestedStatistics{ serviceConfigurationId, statisticNames } },
        std::move(async)
    );
}

HRESULT UserStatisticsService::GetMultipleUserStatisticsForMultipleServiceConfigurations(
    _In_ const Vector<uint64_t>& xuids,
    _In_ const Vector<RequestedStatistics>& requestedServiceConfigurationStatisticsCollection,
    _In_ AsyncContext<Result<Vector<UserStatisticsResult>>> async
) const noexcept
{
    // Set request body to something like:
    //{    
    //    "requestedusers": 
    //    [
    //        "1234567890123460",
    //        "1234567890123234"
    //    ],
    //    "requestedscids": 
    //    [
    //        {
    //            "scid": "c402ff50-3e76-11e2-a25f-0800200c1212",
    //            "requestedstats": 
    //            [
    //                "Game4FirefightKills",
    //                "Game4FirefightHeadshots"
    //            ]
    //        },
    //        {
    //            "scid": "c402ff50-3e76-11e2-a25f-0800200c0343",
    //            "requestedstats": 
    //            [
    //                "OverallGameKills",
    //                "GameHeadshots"
    //            ]
    //        }
    //    ] 
    //}

    RETURN_HR_INVALIDARGUMENT_IF(xuids.empty());
    RETURN_HR_INVALIDARGUMENT_IF(requestedServiceConfigurationStatisticsCollection.empty());

    JsonDocument rootJson{ rapidjson::kObjectType };
    JsonDocument::AllocatorType& allocator{ rootJson.GetAllocator() };

    JsonValue requestedUsersJsonArray{ rapidjson::kArrayType };
    JsonUtils::SerializeVector(JsonUtils::JsonXuidSerializer, xuids, requestedUsersJsonArray, allocator);
    rootJson.AddMember("requestedusers", requestedUsersJsonArray.Move(), allocator);

    //requestedscids
    JsonValue requestedscidsJson{ rapidjson::kArrayType };
    for (const auto& request : requestedServiceConfigurationStatisticsCollection)
    {
        JsonValue requestedJson{ rapidjson::kObjectType };
        JsonValue val;
        val.SetString(request.ServiceConfigurationId().c_str(), allocator);
        requestedJson.AddMember("scid", val, allocator);

        JsonValue requestedstatsJson{ rapidjson::kArrayType };
        for (const auto& stat : request.Statistics())
        {
            JsonValue statValue;
            statValue.SetString(stat.c_str(), allocator);
            requestedstatsJson.PushBack(statValue, allocator);
        }
        requestedJson.AddMember("requestedstats", requestedstatsJson, allocator);

        requestedscidsJson.PushBack(requestedJson, allocator);
    }

    rootJson.AddMember("requestedscids", requestedscidsJson, allocator);

    Result<User> userResult = m_user.Copy();
    RETURN_HR_IF_FAILED(userResult.Hresult());

    auto httpCall = MakeShared<XblHttpCall>(userResult.ExtractPayload());
    RETURN_HR_IF_FAILED(httpCall->Init(
        m_xboxLiveContextSettings,
        "POST",
        XblHttpCall::BuildUrl("userstats", "/batch?operation=read"),
        xbox_live_api::get_multiple_user_statistics_for_multiple_service_configurations
    ));

    RETURN_HR_IF_FAILED(httpCall->SetXblServiceContractVersion(1));
    RETURN_HR_IF_FAILED(httpCall->SetRequestBody(JsonUtils::SerializeJson(rootJson)));

    return httpCall->Perform(AsyncContext<HttpResult>{
        async.Queue().DeriveWorkerQueue(),
            [
                async
            ]
        (HttpResult httpResult)
        {
            HRESULT hr{ Failed(httpResult) ? httpResult.Hresult() : httpResult.Payload()->Result() };
            if (SUCCEEDED(hr))
            {
                Vector<UserStatisticsResult> result;
                hr = JsonUtils::ExtractJsonVector<UserStatisticsResult>(
                    UserStatisticsResult::Deserialize,
                    httpResult.Payload()->GetResponseBodyJson(),
                    "users",
                    result,
                    true
                );

                return async.Complete({ result, hr });
            }
            async.Complete(hr);
        }});
}

XblFunctionContext UserStatisticsService::AddStatisticChangedHandler(
    StatisticChangeHandler handler
) noexcept
{
    std::lock_guard<std::mutex> lock{ m_mutex };

    // Add subs to RTA manager if needed
    if (m_statisticChangeHandlers.empty())
    {
        for (auto& userPair : m_trackedStats)
        {
            for (auto& statPair : userPair.second)
            {
                m_rtaManager->AddSubscription(m_user, statPair.second.subscription);
            }
        }
    }

    m_statisticChangeHandlers[m_nextToken] = std::move(handler);
    return m_nextToken++;
}

void UserStatisticsService::RemoveStatisticChangedHandler(
    XblFunctionContext token
) noexcept
{
    std::lock_guard<std::mutex> lock{ m_mutex };

    auto removed{ m_statisticChangeHandlers.erase(token) };

    // Remove subs if there are no more handlers
    if (removed && m_statisticChangeHandlers.empty())
    {
        for (auto& userPair : m_trackedStats)
        {
            for (auto& statPair : userPair.second)
            {
                m_rtaManager->RemoveSubscription(m_user, statPair.second.subscription);
            }
        }
    }
}

HRESULT UserStatisticsService::TrackStatistics(
    _In_ const Vector<uint64_t> xuids,
    _In_ const String& scid,
    _In_ const Vector<String>& statNames
) noexcept
{
    std::lock_guard<std::mutex> lock{ m_mutex };

    for (auto& xuid : xuids)
    {
        auto& userStats{ m_trackedStats[xuid] };
        for (auto& statName : statNames)
        {
            auto iter{ userStats.find({ scid, statName }) };
            if (iter == userStats.end())
            {
                auto sub{ MakeShared<StatisticChangeSubscription>(xuid, scid, statName, shared_from_this()) };
                userStats[{scid, statName}] = SubscriptionHolder{ 1, sub };

                // If there are existing handlers, add the new subs to RTA manager
                if (!m_statisticChangeHandlers.empty())
                {
                    RETURN_HR_IF_FAILED(m_rtaManager->AddSubscription(m_user, sub));
                }
            }
            else
            {
                ++(iter->second.refCount);
            }
        }
    }
    return S_OK;
}

HRESULT UserStatisticsService::StopTrackingStatistics(
    _In_ const Vector<uint64_t> xuids,
    _In_ const String& scid,
    _In_ const Vector<String>& statNames
) noexcept
{
    std::lock_guard<std::mutex> lock{ m_mutex };

    for (auto& xuid : xuids)
    {
        auto& userStats{ m_trackedStats[xuid] };
        for (auto& statName : statNames)
        {
            auto iter{ userStats.find({ scid, statName }) };
            if (iter != userStats.end() && --(iter->second.refCount) == 0)
            {
                // Remove subs from RTA manager as necessary
                if (!m_statisticChangeHandlers.empty())
                {
                    RETURN_HR_IF_FAILED(m_rtaManager->RemoveSubscription(m_user, iter->second.subscription));
                }
                userStats.erase(iter);
            }
        }
    }
    return S_OK;
}

HRESULT UserStatisticsService::StopTrackingUsers(
    _In_ const Vector<uint64_t>& xuids
) noexcept
{
    std::lock_guard<std::mutex> lock{ m_mutex };

    for (auto& xuid : xuids)
    {
        auto& userStats{ m_trackedStats[xuid] };
        for (auto& statPair : userStats)
        {
            if (--(statPair.second.refCount) == 0)
            {
                // Remove the subs from RTA manager as necessary
                if (!m_statisticChangeHandlers.empty())
                {
                    RETURN_HR_IF_FAILED(m_rtaManager->RemoveSubscription(m_user, statPair.second.subscription));
                }
            }
        }
    }
    return S_OK;
}

void UserStatisticsService::HandleStatisticChanged(
    const StatisticChangeEventArgs& args
) const noexcept
{
    std::unique_lock<std::mutex> lock{ m_mutex };
    auto handlers{ m_statisticChangeHandlers };
    lock.unlock();

    for (auto& pair : handlers)
    {
        pair.second(args);
    }
}

String UserStatisticsService::UserStatsSubpath(
    _In_ uint64_t xuid,
    _In_ const String& serviceConfigurationId,
    _In_ Vector<String> statNames
) noexcept
{
    Stringstream ss;
    ss << "/users/xuid(" << xuid << ")/scids/" << serviceConfigurationId  << "/stats/";

    for (const auto& statName : statNames)
    {
        if (statName != statNames.front())
        {
            ss << ",";
        }
        ss << statName;
    }
    return ss.str();
}

NAMESPACE_MICROSOFT_XBOX_SERVICES_USERSTATISTICS_CPP_END