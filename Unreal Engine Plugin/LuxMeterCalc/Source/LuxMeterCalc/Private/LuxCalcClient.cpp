#include "LuxCalcClient.h"
#include "LuxMeterCalcSettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

void FLuxCalcClient::Health(FHealthCallback OnComplete)
{
	const ULuxMeterCalcSettings* Settings = GetDefault<ULuxMeterCalcSettings>();
	const FString Url = Settings->GetBaseUrl() + TEXT("/health");

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(Settings->RequestTimeoutSeconds);
	Request->OnProcessRequestComplete().BindLambda(
		[Callback = MoveTemp(OnComplete)](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			if (!bSucceeded || !Response.IsValid() || Response->GetResponseCode() != 200)
			{
				Callback(false, FString());
				return;
			}

			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				Callback(false, FString());
				return;
			}

			const bool bOk = Json->GetBoolField(TEXT("ok"));
			FString Version;
			Json->TryGetStringField(TEXT("version"), Version);
			Callback(bOk, Version);
		});

	Request->ProcessRequest();
}

void FLuxCalcClient::Measure(const FLuxMeasureRequest& RequestData, FMeasureCallback OnComplete)
{
	const ULuxMeterCalcSettings* Settings = GetDefault<ULuxMeterCalcSettings>();
	const FString Url = Settings->GetBaseUrl() + TEXT("/measure");

	FString Body;
	if (!FJsonObjectConverter::UStructToJsonObjectString(RequestData, Body, 0, 0))
	{
		FLuxMeasureResponse Err;
		Err.Error = TEXT("Failed to serialize request to JSON");
		OnComplete(false, Err);
		return;
	}

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetTimeout(Settings->RequestTimeoutSeconds);
	Request->SetContentAsString(Body);
	Request->OnProcessRequestComplete().BindLambda(
		[Callback = MoveTemp(OnComplete)](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			FLuxMeasureResponse Result;

			if (!bSucceeded || !Response.IsValid())
			{
				Result.Error = TEXT("Network error or no response");
				Callback(false, Result);
				return;
			}

			const FString ResponseBody = Response->GetContentAsString();
			if (Response->GetResponseCode() != 200)
			{
				Result.Error = FString::Printf(TEXT("HTTP %d: %s"), Response->GetResponseCode(), *ResponseBody);
				Callback(false, Result);
				return;
			}

			if (!FJsonObjectConverter::JsonObjectStringToUStruct(ResponseBody, &Result, 0, 0))
			{
				Result.Error = TEXT("Failed to parse response JSON");
				Callback(false, Result);
				return;
			}

			Callback(true, Result);
		});

	Request->ProcessRequest();
}

void FLuxCalcClient::MeasureBatch(const FLuxMeasureBatchRequest& RequestData, FMeasureBatchCallback OnComplete)
{
	const ULuxMeterCalcSettings* Settings = GetDefault<ULuxMeterCalcSettings>();
	const FString Url = Settings->GetBaseUrl() + TEXT("/measure_batch");

	FString Body;
	if (!FJsonObjectConverter::UStructToJsonObjectString(RequestData, Body, 0, 0))
	{
		FLuxMeasureBatchResponse Err;
		Err.Error = TEXT("Failed to serialize batch request to JSON");
		OnComplete(false, Err);
		return;
	}

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	// Batch requests can be much bigger; allow more time per call.
	Request->SetTimeout(FMath::Max(Settings->RequestTimeoutSeconds, 60.f));
	Request->SetContentAsString(Body);
	Request->OnProcessRequestComplete().BindLambda(
		[Callback = MoveTemp(OnComplete)](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			FLuxMeasureBatchResponse Result;

			if (!bSucceeded || !Response.IsValid())
			{
				Result.Error = TEXT("Network error or no response");
				Callback(false, Result);
				return;
			}

			const FString ResponseBody = Response->GetContentAsString();
			if (Response->GetResponseCode() != 200)
			{
				Result.Error = FString::Printf(TEXT("HTTP %d: %s"), Response->GetResponseCode(), *ResponseBody);
				Callback(false, Result);
				return;
			}

			if (!FJsonObjectConverter::JsonObjectStringToUStruct(ResponseBody, &Result, 0, 0))
			{
				Result.Error = TEXT("Failed to parse batch response JSON");
				Callback(false, Result);
				return;
			}

			Callback(true, Result);
		});

	Request->ProcessRequest();
}
