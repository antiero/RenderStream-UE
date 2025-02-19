#include "RenderStreamSceneSelector.h"
#include "Core.h"
#include "GameFramework/Actor.h"
#include "Engine/TextureRenderTarget2D.h"
#include <string.h>
#include <malloc.h>
#include <algorithm>
#include "RenderStream.h"
#include "RSUCHelpers.inl"

#include "RenderCore/Public/ProfilingDebugging/RealtimeGPUProfiler.h"

RenderStreamSceneSelector::~RenderStreamSceneSelector() = default;

const RenderStreamLink::Schema& RenderStreamSceneSelector::Schema() const
{
    if (!m_schemaMem.empty())
        return *reinterpret_cast<const RenderStreamLink::Schema*>(m_schemaMem.data());
    else
        return m_defaultSchema.schema;
}


void RenderStreamSceneSelector::LoadSchemas(const UWorld& World)
{
    const std::string AssetPath = TCHAR_TO_UTF8(*FPaths::GetProjectFilePath());
    uint32_t nBytes = 0;
    RenderStreamLink::instance().rs_loadSchema(AssetPath.c_str(), nullptr, &nBytes);

    const static int MAX_TRIES = 3;
    int iterations = 0;

    RenderStreamLink::RS_ERROR res = RenderStreamLink::RS_ERROR_BUFFER_OVERFLOW;
    do
    {
        m_schemaMem.resize(nBytes);
        res = RenderStreamLink::instance().rs_loadSchema(AssetPath.c_str(), reinterpret_cast<RenderStreamLink::Schema*>(m_schemaMem.data()), &nBytes);

        if (res == RenderStreamLink::RS_ERROR_SUCCESS)
            break;

        ++iterations;
    } while (res == RenderStreamLink::RS_ERROR_BUFFER_OVERFLOW && iterations < MAX_TRIES);

    bool loaded = true;
    if (res == RenderStreamLink::RS_ERROR_SUCCESS)
    {
        if (!OnLoadedSchema(World, Schema()))
        {
            UE_LOG(LogRenderStream, Error, TEXT("Incomptible schema"));
            loaded = false;
        }
    }
    else
    {
        UE_LOG(LogRenderStream, Error, TEXT("Unable to load schema - error %d"), res);
        loaded = false;
    }

    // Failed the above, get something set to be in a valid state.
    if (!loaded)
    {
        m_schemaMem.clear();
        m_defaultSchema.reset();
        RenderStreamLink::Schema& Schema = m_defaultSchema.schema;
        Schema.scenes.nScenes = 1;
        Schema.scenes.scenes = static_cast<RenderStreamLink::RemoteParameters*>(malloc(Schema.scenes.nScenes * sizeof(RenderStreamLink::RemoteParameters)));
        Schema.scenes.scenes[0].name = _strdup("Default");
        Schema.scenes.scenes[0].nParameters = 0;
        Schema.scenes.scenes[0].parameters = nullptr;
        res = RenderStreamLink::instance().rs_setSchema(&Schema);
        if (res != RenderStreamLink::RS_ERROR_SUCCESS)
            UE_LOG(LogRenderStream, Error, TEXT("Unable to set default schema - error %d"), res);
    }
}


static bool validateField(FString key_, FString undecoratedSuffix, RenderStreamLink::RemoteParameterType expectedType, const RenderStreamLink::RemoteParameter& parameter)
{
    FString key = key_ + (undecoratedSuffix.IsEmpty() ? "" : "_" + undecoratedSuffix);

    if (key != parameter.key)
    {
        UE_LOG(LogRenderStream, Error, TEXT("Parameter mismatch - expected key %s, got %s"), UTF8_TO_TCHAR(parameter.key), *key);
        return false;
    }

    if (expectedType != parameter.type)
    {
        UE_LOG(LogRenderStream, Error, TEXT("Parameter mismatch - expected type %d, got %d"), parameter.type, expectedType);
        return false;
    }
    return true;
}

bool RenderStreamSceneSelector::ValidateParameters(const RenderStreamLink::RemoteParameters& sceneParameters, std::initializer_list<const AActor*> Actors) const
{
    size_t offset = 0;

    for (const AActor* actor : Actors)
    {
        if (!actor)
            continue; // it's convenient at the higher level to pass nulls if there's a pattern which can miss pieces

        size_t increment = ValidateParameters(actor, sceneParameters.parameters + offset, sceneParameters.nParameters);
        if (increment == SIZE_MAX)
        {
            UE_LOG(LogRenderStream, Error, TEXT("Schema validation failed for actor '%s'"), *actor->GetName());
            return false;
        }
        offset += increment;
    }

    if (offset < sceneParameters.nParameters)
    {
        UE_LOG(LogRenderStream, Error, TEXT("Unexpected extra parameters in schema"));
        return false;
    }

    return true;
}

size_t RenderStreamSceneSelector::ValidateParameters(const AActor* Root, RenderStreamLink::RemoteParameter* const parameters, size_t numParameters) const
{
    size_t nParameters = 0;

    for (TFieldIterator<FProperty> PropIt(Root->GetClass(), EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
    {
        const FProperty* Property = *PropIt;
        const FString Name = Property->GetName();
        if (!Property->HasAllPropertyFlags(CPF_Edit | CPF_BlueprintVisible) || Property->HasAllPropertyFlags(CPF_DisableEditOnInstance))
        {
            UE_LOG(LogRenderStream, Verbose, TEXT("Unexposed property: %s"), *Name);
        }
        else if (const FBoolProperty* BoolProperty = CastField<const FBoolProperty>(Property))
        {
            UE_LOG(LogRenderStream, Log, TEXT("Exposed bool property: %s"), *Name);
            if (numParameters < nParameters + 1)
            {
                UE_LOG(LogRenderStream, Error, TEXT("Property %s not exposed in schema"), *Name);
                return SIZE_MAX;
            }
            if (!validateField(Name, "", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters]))
                return SIZE_MAX;
            ++nParameters;
        }
        else if (const FByteProperty* ByteProperty = CastField<const FByteProperty>(Property))
        {
            UE_LOG(LogRenderStream, Log, TEXT("Exposed int property: %s"), *Name);
            if (numParameters < nParameters + 1)
            {
                UE_LOG(LogRenderStream, Error, TEXT("Property %s not exposed in schema"), *Name);
                return SIZE_MAX;
            }
            if (!validateField(Name, "", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters]))
                return SIZE_MAX;
            ++nParameters;
        }
        else if (const FIntProperty* IntProperty = CastField<const FIntProperty>(Property))
        {
            UE_LOG(LogRenderStream, Log, TEXT("Exposed int property: %s"), *Name);
            if (numParameters < nParameters + 1)
            {
                UE_LOG(LogRenderStream, Error, TEXT("Property %s not exposed in schema"), *Name);
                return SIZE_MAX;
            }
            if (!validateField(Name, "", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters]))
                return SIZE_MAX;
            ++nParameters;
        }
        else if (const FFloatProperty* FloatProperty = CastField<const FFloatProperty>(Property))
        {
            UE_LOG(LogRenderStream, Log, TEXT("Exposed float property: %s"), *Name);
            if (numParameters < nParameters + 1)
            {
                UE_LOG(LogRenderStream, Error, TEXT("Property %s not exposed in schema"), *Name);
                return SIZE_MAX;
            }
            if (!validateField(Name, "", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters]))
                return SIZE_MAX;
            ++nParameters;
        }
        else if (const FStructProperty* StructProperty = CastField<const FStructProperty>(Property))
        {
            const void* StructAddress = StructProperty->ContainerPtrToValuePtr<void>(Root);
            if (StructProperty->Struct == TBaseStructure<FVector>::Get())
            {
                UE_LOG(LogRenderStream, Log, TEXT("Exposed vector property: %s"), *Name);
                if (numParameters < nParameters + 3)
                {
                    UE_LOG(LogRenderStream, Error, TEXT("Properties for %s not exposed in schema"), *Name);
                    return SIZE_MAX;
                }
                if (!validateField(Name, "x", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 0]) ||
                    !validateField(Name, "y", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 1]) ||
                    !validateField(Name, "z", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 2]))
                {
                    return SIZE_MAX;
                }
                nParameters += 3;
            }
            else if (StructProperty->Struct == TBaseStructure<FColor>::Get())
            {
                UE_LOG(LogRenderStream, Log, TEXT("Exposed colour property: %s"), *Name);
                if (numParameters < nParameters + 4)
                {
                    UE_LOG(LogRenderStream, Error, TEXT("Properties for %s not exposed in schema"), *Name);
                    return SIZE_MAX;
                }
                if (!validateField(Name, "r", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 0]) ||
                    !validateField(Name, "g", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 1]) ||
                    !validateField(Name, "b", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 2]) ||
                    !validateField(Name, "a", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 3]))
                {
                    return SIZE_MAX;
                }
                nParameters += 4;
            }
            else if (StructProperty->Struct == TBaseStructure<FLinearColor>::Get())
            {
                UE_LOG(LogRenderStream, Log, TEXT("Exposed linear colour property: %s"), *Name);
                if (numParameters < nParameters + 4)
                {
                    UE_LOG(LogRenderStream, Error, TEXT("Properties for %s not exposed in schema"), *Name);
                    return SIZE_MAX;
                }
                if (!validateField(Name, "r", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 0]) ||
                    !validateField(Name, "g", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 1]) ||
                    !validateField(Name, "b", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 2]) ||
                    !validateField(Name, "a", RenderStreamLink::RS_PARAMETER_NUMBER, parameters[nParameters + 3]))
                {
                    return SIZE_MAX;
                }
                nParameters += 4;
            }
            else if (StructProperty->Struct == TBaseStructure<FTransform>::Get())
            {
                UE_LOG(LogRenderStream, Log, TEXT("Exposed transform property: %s"), *Name);
                if (numParameters < nParameters + 1)
                {
                    UE_LOG(LogRenderStream, Error, TEXT("Properties for %s not exposed in schema"), *Name);
                    return SIZE_MAX;
                }
                validateField(Name, "", RenderStreamLink::RS_PARAMETER_TRANSFORM, parameters[nParameters]);
                ++nParameters;
            }
            else
            {
                UE_LOG(LogRenderStream, Warning, TEXT("Unknown struct property: %s"), *Name);
            }
        }
        else if (const FObjectProperty* ObjectProperty = CastField<const FObjectProperty>(Property))
        {
            const void* ObjectAddress = ObjectProperty->ContainerPtrToValuePtr<void>(Root);
            UObject* o = ObjectProperty->GetObjectPropertyValue(ObjectAddress);
            if (UTextureRenderTarget2D* Texture = Cast<UTextureRenderTarget2D>(o))
            {
                UE_LOG(LogRenderStream, Log, TEXT("Exposed render texture property: %s"), *Name);
                if (numParameters < nParameters + 1)
                {
                    UE_LOG(LogRenderStream, Error, TEXT("Properties for %s not exposed in schema"), *Name);
                    return SIZE_MAX;
                }
                validateField(Name, "", RenderStreamLink::RS_PARAMETER_IMAGE, parameters[nParameters]);
                ++nParameters;
            }
            else
            {
                UE_LOG(LogRenderStream, Warning, TEXT("Unknown object property: %s"), *Name);
            }
        }
        else if (const FTextProperty* TextProperty = CastField<const FTextProperty>(Property))
        {
            UE_LOG(LogRenderStream, Log, TEXT("Exposed text property: %s"), *Name);
            if (numParameters < nParameters + 1)
            {
                UE_LOG(LogRenderStream, Error, TEXT("Properties for %s not exposed in schema"), *Name);
                return SIZE_MAX;
            }
            validateField(Name, "", RenderStreamLink::RS_PARAMETER_TEXT, parameters[nParameters]);
            ++nParameters;
        }
        else
        {
            UE_LOG(LogRenderStream, Warning, TEXT("Unsupported exposed property: %s"), *Name);
        }
    }

    return nParameters;
}

void RenderStreamSceneSelector::ApplyParameters(uint32_t sceneId, std::initializer_list<AActor*> Actors) const
{
    check(sceneId < Schema().scenes.nScenes);
    const RenderStreamLink::RemoteParameters& params = Schema().scenes.scenes[sceneId];

    size_t nFloatParams = 0;
    size_t nImageParams = 0;
    size_t nTextParams = 0;
    for (size_t i = 0; i < params.nParameters ; ++i)
    {
        const RenderStreamLink::RemoteParameter& param = params.parameters[i];
        switch (param.type)
        {
        case RenderStreamLink::RS_PARAMETER_NUMBER:
            nFloatParams++;
            break;
        case RenderStreamLink::RS_PARAMETER_IMAGE:
            nImageParams++;
            break;
        case RenderStreamLink::RS_PARAMETER_POSE:
        case RenderStreamLink::RS_PARAMETER_TRANSFORM:
            nFloatParams += 16;
            break;
        case RenderStreamLink::RS_PARAMETER_TEXT:
            nTextParams++;
            break;
        default:
            UE_LOG(LogRenderStream, Error, TEXT("Unhandled parameter type"));
            return;
        }
    }
    
    std::vector<float> floatValues(nFloatParams);
    std::vector<RenderStreamLink::ImageFrameData> imageValues(nImageParams);

    RenderStreamLink::RS_ERROR res = RenderStreamLink::instance().rs_getFrameParameters(params.hash, floatValues.data(), floatValues.size() * sizeof(float));
    if (res != RenderStreamLink::RS_ERROR_SUCCESS)
    {
        UE_LOG(LogRenderStream, Error, TEXT("Unable to get float frame parameters - %d"), res);
        return;
    }
    res = RenderStreamLink::instance().rs_getFrameImageData(params.hash, imageValues.data(), imageValues.size());
    if (res != RenderStreamLink::RS_ERROR_SUCCESS)
    {
        UE_LOG(LogRenderStream, Error, TEXT("Unable to get image frame parameters - %d"), res);
        return;
    }

    // These are updated by ApplyParameters to allow each actor to operate on the next set of data.
    const RenderStreamLink::RemoteParameter* paramsPtr = params.parameters;
    const float* floatValuesPtr = floatValues.data();
    const RenderStreamLink::ImageFrameData* imageValuesPtr = imageValues.data();
    for (AActor* actor : Actors)
    {
        if (!actor)
            continue; // it's convenient at the higher level to pass nulls if there's a pattern which can miss pieces
        ApplyParameters(actor, params.hash, &paramsPtr, &floatValuesPtr, &imageValuesPtr);
    }
}

void RenderStreamSceneSelector::ApplyParameters(AActor* Root, uint64_t specHash, const RenderStreamLink::RemoteParameter** ppParams, const float** ppFloatValues, const RenderStreamLink::ImageFrameData** ppImageValues) const
{
    auto toggle = FHardwareInfo::GetHardwareInfo(NAME_RHI);
    struct
    {
        RenderStreamLink::RSPixelFormat fmt;
        EPixelFormat ue;
    } formatMap[] = {
        // NB. FTextureRenderTargetResource::IsSupportedFormat
        { RenderStreamLink::RS_FMT_INVALID, EPixelFormat::PF_Unknown },
        { RenderStreamLink::RS_FMT_BGRA8, EPixelFormat::PF_B8G8R8A8 },
        { RenderStreamLink::RS_FMT_BGRX8, EPixelFormat::PF_B8G8R8A8 },
        { RenderStreamLink::RS_FMT_RGBA32F, EPixelFormat::PF_FloatRGBA},
        { RenderStreamLink::RS_FMT_RGBA16, EPixelFormat::PF_A16B16G16R16 },
    };

    size_t iParam = 0;
    const RenderStreamLink::RemoteParameter* params = *ppParams;
    size_t iFloat = 0;
    const float* floatValues = *ppFloatValues;
    size_t iImage = 0;
    const RenderStreamLink::ImageFrameData* imageValues = *ppImageValues;
    size_t iText = 0;
    for (TFieldIterator<FProperty> PropIt(Root->GetClass(), EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
    {
        FProperty* Property = *PropIt;
        if (!Property->HasAllPropertyFlags(CPF_Edit | CPF_BlueprintVisible) || Property->HasAllPropertyFlags(CPF_DisableEditOnInstance))
        {
            continue;
        }
        else if (const FBoolProperty* BoolProperty = CastField<const FBoolProperty>(Property))
        {
            const bool v = bool(floatValues[iFloat]);
            BoolProperty->SetPropertyValue_InContainer(Root, v);
            ++iFloat;
        }
        else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
        {
            const uint8 v = uint8(floatValues[iFloat]);
            ByteProperty->SetPropertyValue_InContainer(Root, v);
            ++iFloat;
        }
        else if (FIntProperty* IntProperty = CastField<FIntProperty>(Property))
        {
            const int32 v = int(floatValues[iFloat]);
            IntProperty->SetPropertyValue_InContainer(Root, v);
            ++iFloat;
        }
        else if (FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
        {
            const float v = floatValues[iFloat];
            FloatProperty->SetPropertyValue_InContainer(Root, v);
            ++iFloat;
        }
        else if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            void* StructAddress = StructProperty->ContainerPtrToValuePtr<void>(Root);
            if (StructProperty->Struct == TBaseStructure<FVector>::Get())
            {
                FVector v(floatValues[iFloat], floatValues[iFloat + 1], floatValues[iFloat + 2]);
                StructProperty->CopyCompleteValue(StructAddress, &v);
                iFloat += 3;
            }
            else if (StructProperty->Struct == TBaseStructure<FColor>::Get())
            {
                FColor v(floatValues[iFloat] * 255, floatValues[iFloat + 1] * 255, floatValues[iFloat + 2] * 255, floatValues[iFloat + 3] * 255);
                StructProperty->CopyCompleteValue(StructAddress, &v);
                iFloat += 4;
            }
            else if (StructProperty->Struct == TBaseStructure<FLinearColor>::Get())
            {
                FLinearColor v(floatValues[iFloat], floatValues[iFloat + 1], floatValues[iFloat + 2], floatValues[iFloat + 3]);
                StructProperty->CopyCompleteValue(StructAddress, &v);
                iFloat += 4;
            }
            else if (StructProperty->Struct == TBaseStructure<FTransform>::Get())
            {
                static const FMatrix YUpMatrix(FVector(0.0f, 0.0f, 1.0f), FVector(1.0f, 0.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f), FVector(0.0f, 0.0f, 0.0f));
                static const FMatrix YUpMatrixInv(YUpMatrix.Inverse());

                const FMatrix m(
                    FPlane(floatValues[iFloat + 0], floatValues[iFloat + 1], floatValues[iFloat + 2], floatValues[iFloat + 3]),
                    FPlane(floatValues[iFloat + 4], floatValues[iFloat + 5], floatValues[iFloat + 6], floatValues[iFloat + 7]),
                    FPlane(floatValues[iFloat + 8], floatValues[iFloat + 9], floatValues[iFloat + 10], floatValues[iFloat + 11]),
                    FPlane(floatValues[iFloat + 12], floatValues[iFloat + 13], floatValues[iFloat + 14], floatValues[iFloat + 15])
                );
                FTransform v(YUpMatrix * m * YUpMatrixInv);
                v.ScaleTranslation(FUnitConversion::Convert(1.f, EUnit::Meters, FRenderStreamModule::distanceUnit()));
                StructProperty->CopyCompleteValue(StructAddress, &v);
                iFloat += 16;
            }
        }
        else if (const FObjectProperty* ObjectProperty = CastField<const FObjectProperty>(Property))
        {
            const void* ObjectAddress = ObjectProperty->ContainerPtrToValuePtr<void>(Root);
            UObject* o = ObjectProperty->GetObjectPropertyValue(ObjectAddress);
            if (UTextureRenderTarget2D* Texture = Cast<UTextureRenderTarget2D>(o))
            {
                const RenderStreamLink::ImageFrameData& frameData = imageValues[iImage];
                if (!Texture->bGPUSharedFlag || Texture->GetFormat() != formatMap[frameData.format].ue)
                {
                    Texture->bGPUSharedFlag = true;
                    Texture->InitCustomFormat(frameData.width, frameData.height, formatMap[frameData.format].ue, false);
                }
                else
                {
                    Texture->ResizeTarget(frameData.width, frameData.height);
                }

                ENQUEUE_RENDER_COMMAND(GetTex)(
                [this, toggle, Texture, frameData, iImage](FRHICommandListImmediate& RHICmdList)
                {
                    SCOPED_DRAW_EVENTF(RHICmdList, MediaCapture, TEXT("RS Tex Param Block %d"), iImage);
                    auto rtResource = Texture->GetRenderTargetResource();
                    if (!rtResource)
                    {
                        return;
                    }
                    void* resource = rtResource->TextureRHI->GetNativeResource();

                    RenderStreamLink::SenderFrameTypeData data = { 0 };
                    if (toggle == "D3D11")
                    {
                        data.dx11.resource = static_cast<ID3D11Resource*>(resource);
                        auto err = RenderStreamLink::instance().rs_getFrameImage(frameData.imageId, RenderStreamLink::SenderFrameType::RS_FRAMETYPE_DX11_TEXTURE, data);
                    }
                    else if (toggle == "D3D12")
                    {
                        {
                            SCOPED_DRAW_EVENTF(RHICmdList, MediaCapture, TEXT("RS Tex Param Flush"));
                            RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
                        }

                        data.dx12.resource = static_cast<ID3D12Resource*>(resource);
                        
                        SCOPED_DRAW_EVENTF(RHICmdList, MediaCapture, TEXT("RS getFrameImage %d"), iImage);
                        if (RenderStreamLink::instance().rs_getFrameImage(frameData.imageId, RenderStreamLink::SenderFrameType::RS_FRAMETYPE_DX12_TEXTURE, data) != RenderStreamLink::RS_ERROR_SUCCESS)
                        {

                        }
                        
                    }
                    else
                    {
                        UE_LOG(LogRenderStream, Error, TEXT("RenderStream tried to send frame with unsupported RHI backend."));
                        return;
                    }

                });

                ++iImage;
            }
        }
        else if (const FTextProperty* TextProperty = CastField<const FTextProperty>(Property))
        {
            const char* cString = nullptr;
            if (RenderStreamLink::instance().rs_getFrameText(specHash, iText, &cString) == RenderStreamLink::RS_ERROR_SUCCESS)
            {
                TextProperty->SetPropertyValue_InContainer(Root, FText::FromString(UTF8_TO_TCHAR(cString)));
            }
            ++iText;
        }

        ++iParam;
    }
    
    *ppFloatValues += iFloat;
    *ppImageValues += iImage;
    *ppParams += iParam;

}
