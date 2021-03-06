﻿/*
 * All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
 * its licensors.
 *
 * For complete copyright and license terms please see the LICENSE at the root of this
 * distribution (the "License"). All use of this software is governed by the License,
 * or, if provided, by the license below or the license accompanying this file. Do not
 * remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *
 */

#include <PhysX_precompiled.h>

#include "EditorTerrainComponent.h"
#include <AzCore/Serialization/EditContext.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <PhysX/SystemComponentBus.h>
#include <Source/Pipeline/HeightFieldAssetHandler.h>
#include <Source/Utils.h>
#include <Source/EditorSystemComponent.h>
#include <AzFramework/Physics/SystemBus.h>
#include <AzFramework/Physics/World.h>
#include <AzFramework/Asset/AssetSystemBus.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/UI/PropertyEditor/PropertyEditorAPI.h>

#include <IEditor.h>
#include <I3DEngine.h>

namespace PhysX
{
    static const float MaxCryTerrainHeight = 1024.f;
    static const float MaxPhysxTerrainHeight = 32767.0f;

    namespace TerrainUtils
    {
        static int GetMaterialIndexForSurfaceId(int surfaceId, AZStd::vector<int>& mapping)
        {
            for (int materialIndex = 0; materialIndex < mapping.size(); ++materialIndex)
            {
                if (mapping[materialIndex] == surfaceId)
                {
                    return materialIndex;
                }
            }
            mapping.push_back(surfaceId);
            return static_cast<int>(mapping.size()) - 1;
        }

        AZStd::string GetLevelFolder()
        {
            IEditor* editor = nullptr;
            AzToolsFramework::EditorRequests::Bus::BroadcastResult(editor, &AzToolsFramework::EditorRequests::GetEditor);
            if (editor)
            {
                AZStd::string levelFolder = editor->GetLevelName().toStdString().c_str();
                AzFramework::StringFunc::Path::Join("Levels", levelFolder.c_str(), levelFolder);
                return levelFolder;
            }
            else
            {
                return "";
            }
        }

        bool CheckSourceControl(const char* filename)
        {
            using ApplicationBus = AzToolsFramework::ToolsApplicationRequestBus;

            char path[AZ_MAX_PATH_LEN];
            path[0] = '\0';
            AZ::IO::FileIOBase::GetInstance()->ResolvePath(filename, path, AZ_MAX_PATH_LEN);

            bool checkedOutSuccessfully = false;
            ApplicationBus::BroadcastResult
            (
                checkedOutSuccessfully
                , &ApplicationBus::Events::RequestEditForFileBlocking
                , path
                , "Checking out for edit..."
                , ApplicationBus::Events::RequestEditProgressCallback()
            );

            return checkedOutSuccessfully;
        }

        bool CreateTargetDirectory(const AZStd::string& path)
        {
            AZStd::string folderPath;
            AzFramework::StringFunc::Path::GetFolderPath(path.c_str(), folderPath);
            if (!AZ::IO::FileIOBase::GetInstance()->CreatePath(folderPath.c_str()))
            {
                AZ_Warning("", false, "Level exporting failed because the output directory \"%s\" could not be created.", folderPath.c_str());
                return false;
            }

            // Convert to absolute path
            if (!CheckSourceControl(path.c_str()))
            {
                AZ_Error("EditorTerrainComponent", false, "LevelExporter - Failed to write \"%s\", source control checkout failed", path.c_str());
                return false;
            }

            return true;
        }

        AZ::Vector3 GetScale()
        {
            IEditor* editor = nullptr;
            AzToolsFramework::EditorRequests::Bus::BroadcastResult(editor, &AzToolsFramework::EditorRequests::GetEditor);

            // Scale back up the height when instancing the heightfield into the world
            float heightScale = MaxCryTerrainHeight / MaxPhysxTerrainHeight;
            float rowScale = editor->Get3DEngine()->GetHeightMapUnitSize();
            float colScale = editor->Get3DEngine()->GetHeightMapUnitSize();
            return AZ::Vector3(rowScale, colScale, heightScale);
        }
    }

    void EditorTerrainComponent::Reflect(AZ::ReflectContext* context)
    {
        if (auto serializeContext = azrtti_cast<AZ::SerializeContext*>(context))
        {
            serializeContext->Class<EditorTerrainComponent, EditorComponentBase>()
                ->Version(1)
                ->Field("Configuration", &EditorTerrainComponent::m_configuration)
                ->Field("ExportedAssetPath", &EditorTerrainComponent::m_exportAssetPath)
                ->Field("ExportOnSave", &EditorTerrainComponent::m_exportOnSave)
                ->Field("TerrainInEditor", &EditorTerrainComponent::m_createTerrainInEditor)
            ;

            if (auto editContext = serializeContext->GetEditContext())
            {
                // EditorTerrainComponent
                editContext->Class<EditorTerrainComponent>(
                    "PhysX Terrain", "Terrain Physics")
                    ->ClassElement(AZ::Edit::ClassElements::EditorData, "")
                    ->Attribute(AZ::Edit::Attributes::Category, "PhysX")
                    ->Attribute(AZ::Edit::Attributes::Icon, "Editor/Icons/Components/PhysXTerrain.svg")
                    ->Attribute(AZ::Edit::Attributes::ViewportIcon, "Editor/Icons/Components/PhysXTerrain.svg")
                    ->Attribute(AZ::Edit::Attributes::AppearsInAddComponentMenu, AZ_CRC("Game", 0x232b318c))
                    ->Attribute(AZ::Edit::Attributes::AutoExpand, true)
                    ->DataElement(AZ::Edit::UIHandlers::Default, &EditorTerrainComponent::m_configuration, "Configuration", "Terrain configuration")
                    ->Attribute(AZ::Edit::Attributes::Visibility, AZ::Edit::PropertyVisibility::ShowChildrenOnly)
                    ->Attribute(AZ::Edit::Attributes::ChangeNotify, &EditorTerrainComponent::UpdateHeightFieldAsset)
                    ->DataElement(AZ::Edit::UIHandlers::Default, &EditorTerrainComponent::m_exportOnSave, "Export On Save", "Cry terrain will be exported to an asset when the level is saved")
                    ->DataElement(AZ::Edit::UIHandlers::Default, &EditorTerrainComponent::m_createTerrainInEditor, "Terrain In Editor", "If true, terrain will be added to the editor world")
                    ->Attribute(AZ::Edit::Attributes::ChangeNotify, &EditorTerrainComponent::CreateEditorTerrain)
                ;
            }
        }
    }

    void EditorTerrainComponent::Activate()
    {
        AzToolsFramework::Components::EditorComponentBase::Activate();
        PhysX::EditorTerrainComponentRequestsBus::Handler::BusConnect(GetEntityId());

        Physics::EditorTerrainMaterialRequestsBus::Handler::BusConnect();
        AzToolsFramework::EntitySelectionEvents::Bus::Handler::BusConnect(GetEntityId());

        // If this component is newly created, it won't have an asset id assigned yet, so export immediately.
        if (!m_configuration.m_heightFieldAsset.GetId().IsValid())
        {
            ExportTerrain();
        }

        UpdateHeightFieldAsset();
        RegisterForEditorEvents();
        CreateEditorTerrain();
    }

    void EditorTerrainComponent::Deactivate()
    {
        AzToolsFramework::EntitySelectionEvents::Bus::Handler::BusDisconnect();
        PhysX::EditorTerrainComponentRequestsBus::Handler::BusDisconnect();
        Physics::EditorTerrainMaterialRequestsBus::Handler::BusDisconnect();

        UnregisterForEditorEvents();
        AzToolsFramework::Components::EditorComponentBase::Deactivate();
        m_editorTerrain.reset(nullptr);
    }

    void EditorTerrainComponent::BuildGameEntity(AZ::Entity* gameEntity)
    {
        gameEntity->CreateComponent<TerrainComponent>(m_configuration);
    }

    void EditorTerrainComponent::RegisterForEditorEvents()
    {
        IEditor* editor = nullptr;
        AzToolsFramework::EditorRequests::Bus::BroadcastResult(editor, &AzToolsFramework::EditorRequests::GetEditor);
        editor->RegisterNotifyListener(this);
    }

    void EditorTerrainComponent::UnregisterForEditorEvents()
    {
        IEditor* editor = nullptr;
        AzToolsFramework::EditorRequests::Bus::BroadcastResult(editor, &AzToolsFramework::EditorRequests::GetEditor);
        editor->UnregisterNotifyListener(this);
    }

    void EditorTerrainComponent::OnEditorNotifyEvent(EEditorNotifyEvent editorEvent)
    {
        switch (editorEvent)
        {
        case eNotify_OnEndSceneSave:
            if (m_exportOnSave)
            {
                ExportTerrain();
                CreateEditorTerrain();
            }
            break;
        }
    }

    void EditorTerrainComponent::ExportTerrain()
    {
        AZStd::string exportPath = GetExportPath();

        if (!TerrainUtils::CreateTargetDirectory(exportPath))
        {
            AZ_Warning("EditorTerrainComponent", false, "Failed to export terrain at: %s", exportPath.c_str())
            return;
        }

        m_configuration.m_heightFieldAsset = ExportHeightFieldAsset(exportPath);
    }

    void EditorTerrainComponent::UpdateHeightFieldAsset()
    {
        if (m_configuration.m_heightFieldAsset.GetId().IsValid())
        {
            if (m_configuration.m_heightFieldAsset.GetStatus() == AZ::Data::AssetData::AssetStatus::Error ||
                m_configuration.m_heightFieldAsset.GetStatus() == AZ::Data::AssetData::AssetStatus::NotLoaded)
            {
                m_configuration.m_heightFieldAsset.QueueLoad();
            }
            AZ::Data::AssetBus::Handler::BusConnect(m_configuration.m_heightFieldAsset.GetId());
        }
    }

    void EditorTerrainComponent::OnAssetReady(AZ::Data::Asset<AZ::Data::AssetData> asset)
    {
        if (asset == m_configuration.m_heightFieldAsset)
        {
            m_configuration.m_heightFieldAsset = asset;
            CreateEditorTerrain();
        }
    }

    void EditorTerrainComponent::OnAssetReloaded(AZ::Data::Asset<AZ::Data::AssetData> asset)
    {
        if (asset == m_configuration.m_heightFieldAsset)
        {
            m_configuration.m_heightFieldAsset = asset;
            CreateEditorTerrain();
        }
    }

    AZStd::string EditorTerrainComponent::GetExportPath()
    {
        AZStd::string levelDataFolder = TerrainUtils::GetLevelFolder();

        AZStd::string fullPath;
        AzFramework::StringFunc::Path::Join("@devassets@", levelDataFolder.c_str(), fullPath);
        AzFramework::StringFunc::Path::Join(fullPath.c_str(), m_exportAssetPath.c_str(), fullPath);
        return fullPath;
    }

    AZ::Data::Asset<Pipeline::HeightFieldAsset> EditorTerrainComponent::ExportHeightFieldAsset(const AZStd::string& relativePath)
    {
        char path[AZ_MAX_PATH_LEN];
        path[0] = '\0';
        AZ::IO::FileIOBase::GetInstance()->ResolvePath(relativePath.c_str(), path, AZ_MAX_PATH_LEN);

        IEditor* editor = nullptr;
        AzToolsFramework::EditorRequests::Bus::BroadcastResult(editor, &AzToolsFramework::EditorRequests::GetEditor);

        // Cry adds on an extra col and row at the edge of the terrain.
        // So for a terrain size of 1024, we actually need 1025 samples.
        int32_t terrainSize = editor->Get3DEngine()->GetTerrainSize() + 1;
        AZStd::vector<physx::PxHeightFieldSample> pxSamples(terrainSize * terrainSize);

        m_configuration.m_terrainSurfaceIdIndexMapping.clear();
        m_configuration.m_terrainSurfaceIdIndexMapping.reserve(50);

        for (int32_t y = 0; y < terrainSize; ++y)
        {
            for (int32_t x = 0; x < terrainSize; ++x)
            {
                // At the edge of the terrain, we need to query the adjacent
                // row/col. As Cry will return 0 if it's outside the terrain size bounds.
                int cryIndexX = AZStd::clamp(x, 0, terrainSize - 2);
                int cryIndexY = AZStd::clamp(y, 0, terrainSize - 2);

                float terrainHeight = editor->Get3DEngine()->GetTerrainZ(cryIndexX, cryIndexY);
                int surfaceId = editor->Get3DEngine()->GetTerrainSurfaceId(cryIndexX, cryIndexY);

                int materialIndex = TerrainUtils::GetMaterialIndexForSurfaceId(surfaceId, m_configuration.m_terrainSurfaceIdIndexMapping);

                int32_t samplesIndex = y * terrainSize + x;
                physx::PxHeightFieldSample& pxSample = pxSamples[samplesIndex];

                AZ_Warning("EditorTerrainComponent", terrainHeight <= MaxCryTerrainHeight, "Terrain height exceeds max values, there will be physics artifacts");

                // Scale down the height into a 16bit value
                pxSample.height = (physx::PxI16)(MaxPhysxTerrainHeight / MaxCryTerrainHeight * terrainHeight);
                pxSample.materialIndex0 = physx::PxBitAndByte(materialIndex, false);
                pxSample.materialIndex1 = physx::PxBitAndByte(materialIndex, false);
            }
        }

        physx::PxHeightFieldDesc pxHeightFieldDesc;
        pxHeightFieldDesc.format = physx::PxHeightFieldFormat::eS16_TM;
        pxHeightFieldDesc.nbColumns = terrainSize;
        pxHeightFieldDesc.nbRows = terrainSize;
        pxHeightFieldDesc.samples.data = pxSamples.data();
        pxHeightFieldDesc.samples.stride = sizeof(physx::PxHeightFieldSample);

        physx::PxDefaultMemoryOutputStream outputStream;

        physx::PxCooking* cooking = nullptr;
        SystemRequestsBus::BroadcastResult(cooking, &SystemRequests::GetCooking);
        bool success = cooking->cookHeightField(pxHeightFieldDesc, outputStream);
        if (success)
        {
            PhysX::Pipeline::HeightFieldAssetHeader header;
            header.m_assetVersion = 1;
            header.m_assetDataSize = outputStream.getSize();

            physx::PxDefaultFileOutputStream file(path);
            file.write(&header, sizeof(header));
            file.write(outputStream.getData(), outputStream.getSize());
        }

        // Force io sync with asset processor
        AzFramework::AssetSystem::AssetStatus status;
        AzFramework::AssetSystemRequestBus::BroadcastResult(status, &AzFramework::AssetSystem::AssetSystemRequests::GetAssetStatus, path);

        // Find asset id
        bool sourceInfoFound = false;
        AZ::Data::AssetInfo assetInfo;
        AZStd::string watchFolder;
        AzToolsFramework::AssetSystemRequestBus::BroadcastResult(sourceInfoFound, &AzToolsFramework::AssetSystemRequestBus::Events::GetSourceInfoBySourcePath, path, assetInfo, watchFolder);

        // Get asset
        return AZ::Data::AssetManager::Instance().GetAsset<PhysX::Pipeline::HeightFieldAsset>(assetInfo.m_assetId, true, nullptr, false);
    }

    void EditorTerrainComponent::CreateEditorTerrain()
    {
        if (!m_createTerrainInEditor)
        {
            m_editorTerrain = nullptr;
            return;
        }

        if (m_configuration.m_heightFieldAsset.GetStatus() != AZ::Data::AssetData::AssetStatus::Ready)
        {
            // If the asset is not ready, wait for OnAssetReady to be invoked.
            return;
        }

        AZStd::shared_ptr<Physics::World> editorWorld;
        Physics::EditorWorldBus::BroadcastResult(editorWorld, &Physics::EditorWorldRequests::GetEditorWorld);
        if (editorWorld)
        {
            m_configuration.m_scale = TerrainUtils::GetScale();
            m_editorTerrain = Utils::CreateTerrain(m_configuration, GetEntityId(), GetEntity()->GetName().c_str());
            if (m_editorTerrain)
            {
                editorWorld->AddBody(*m_editorTerrain);
            }
        }

        Physics::EditorWorldBus::Broadcast(&Physics::EditorWorldRequests::MarkEditorWorldDirty);
    }

    void EditorTerrainComponent::SetMaterialSelectionForSurfaceId(int surfaceId, const Physics::MaterialSelection& selection)
    {
        m_configuration.m_terrainMaterialsToSurfaceIds[surfaceId] = selection;
    }

    bool EditorTerrainComponent::GetMaterialSelectionForSurfaceId(int surfaceId, Physics::MaterialSelection& selection)
    {
        auto selectionIterator = m_configuration.m_terrainMaterialsToSurfaceIds.find(surfaceId);
        if (selectionIterator != m_configuration.m_terrainMaterialsToSurfaceIds.end())
        {
            selection = selectionIterator->second;
            return true;
        }

        return false;
    }

    void EditorTerrainComponent::GetTrianglesForHeightField(AZStd::vector<AZ::Vector3>& verts, AZStd::vector<AZ::u32>& indices, AZStd::vector<Physics::MaterialId>& materialIds) const
    {
        verts.clear();
        indices.clear();
        materialIds.clear();

        if (m_configuration.m_heightFieldAsset.GetId().IsValid())
        {
            if (m_configuration.m_heightFieldAsset.GetStatus() == AZ::Data::AssetData::AssetStatus::Ready)
            {
                physx::PxHeightField* heightfield = m_configuration.m_heightFieldAsset.Get()->GetHeightField();

                int vertsCount = 0;
                int triCount = 0;

                for (int32_t y = 0; y < heightfield->getNbRows() - 1; ++y)
                {
                    for (int32_t x = 0; x < heightfield->getNbColumns() - 1; ++x)
                    {
                        float terrainHeight00 = heightfield->getHeight(x, y);
                        float terrainHeight10 = heightfield->getHeight(x + 1, y);
                        float terrainHeight01 = heightfield->getHeight(x, y + 1);
                        float terrainHeight11 = heightfield->getHeight(x + 1, y + 1);

                        // TODO: Need to be able to properly map back from the physX index to our MaterialId
                        // Physics::MaterialId materialIndex0(heightfield->getTriangleMaterialIndex(triCount++));
                        // Physics::MaterialId materialIndex1(heightfield->getTriangleMaterialIndex(triCount++));

                        AZ::Vector3 scale = TerrainUtils::GetScale();

                        verts.push_back(AZ::Vector3(float(y), float(x), terrainHeight00) * scale);
                        verts.push_back(AZ::Vector3(float(y + 1), float(x), terrainHeight01) * scale);
                        verts.push_back(AZ::Vector3(float(y), float(x + 1), terrainHeight01) * scale);
                        verts.push_back(AZ::Vector3(float(y + 1), float(x + 1), terrainHeight11) * scale);

                        // Build the first triangle
                        indices.push_back(vertsCount);
                        indices.push_back(vertsCount + 1);
                        indices.push_back(vertsCount + 3);
                        materialIds.push_back(Physics::MaterialId());   // TODO - get actual material ID

                        // Build the second triangle
                        indices.push_back(vertsCount);
                        indices.push_back(vertsCount + 3);
                        indices.push_back(vertsCount + 2);
                        materialIds.push_back(Physics::MaterialId());   // TODO - get actual material ID

                        vertsCount += 4;
                    }
                }

                AZ_Assert(indices.size() % 3 == 0, "Failed to properly build indices to mesh");
                AZ_Assert(materialIds.size() == indices.size() / 3, "Failed to properly build materials on mesh");
            }
        }
    }

    // AzToolsFramework::EntitySelectionEvents
    void EditorTerrainComponent::OnSelected()
    {
        PhysX::ConfigurationNotificationBus::Handler::BusConnect();
    }

    void EditorTerrainComponent::OnDeselected()
    {
        PhysX::ConfigurationNotificationBus::Handler::BusDisconnect();
    }

    // PhysX::ConfigurationNotificationBus
    void EditorTerrainComponent::OnConfigurationRefreshed(const PhysX::Configuration& configuration)
    {
        AZ_UNUSED(configuration);
        AzToolsFramework::PropertyEditorGUIMessages::Bus::Broadcast(&AzToolsFramework::PropertyEditorGUIMessages::RequestRefresh,
            AzToolsFramework::PropertyModificationRefreshLevel::Refresh_AttributesAndValues);
    }
}
