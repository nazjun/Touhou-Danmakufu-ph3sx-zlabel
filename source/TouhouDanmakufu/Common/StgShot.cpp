#include "source/GcLib/pch.h"

#include "StgShot.hpp"
#include "StgSystem.hpp"
#include "StgIntersection.hpp"
#include "StgItem.hpp"
#include "../../GcLib/directx/HLSL.hpp"

//****************************************************************************
//StgShotManager
//****************************************************************************
StgShotManager::StgShotManager(StgStageController* stageController) {
	stageController_ = stageController;

	listPlayerShotData_ = std::make_unique<StgShotDataList>();
	listEnemyShotData_ = std::make_unique<StgShotDataList>();

	rcDeleteClip_ = DxRect<LONG>(-64, -64, 64, 64);

	filterMin_ = D3DTEXF_LINEAR;
	filterMag_ = D3DTEXF_LINEAR;

	{
		RenderShaderLibrary* shaderManager_ = ShaderManager::GetBase()->GetRenderLib();
		effectShot_ = shaderManager_->GetRender2DShader();
	}
	{
		size_t renderPriMax = stageController_->GetMainObjectManager()->GetRenderBucketCapacity();

		listRenderQueuePlayer_.resize(renderPriMax);
		listRenderQueueEnemy_.resize(renderPriMax);
		for (size_t i = 0; i < renderPriMax; ++i) {
			listRenderQueuePlayer_[i].listShot.resize(32);
			listRenderQueueEnemy_[i].listShot.resize(32);
		}
	}

	SetDeleteEventEnableByType(StgStageItemScript::EV_DELETE_SHOT_IMMEDIATE, true);
	SetDeleteEventEnableByType(StgStageItemScript::EV_DELETE_SHOT_FADE, true);
	SetDeleteEventEnableByType(StgStageItemScript::EV_DELETE_SHOT_TO_ITEM, true);
}
StgShotManager::~StgShotManager() {
	for (ref_unsync_ptr<StgShotObject>& obj : listObj_) {
		if (obj)
			obj->ClearShotObject();
	}
}
void StgShotManager::Work() {
	for (auto itr = listObj_.begin(); itr != listObj_.end(); ) {
		ref_unsync_ptr<StgShotObject>& obj = *itr;
		if (obj->IsDeleted()) {
			obj->ClearShotObject();
			itr = listObj_.erase(itr);
		}
		else if (!obj->IsActive()) {
			itr = listObj_.erase(itr);
		}
		else ++itr;
	}
}

std::array<BlendMode, StgShotManager::BLEND_COUNT> StgShotManager::blendTypeRenderOrder = {
	MODE_BLEND_ADD_ARGB,
	MODE_BLEND_ADD_RGB,
	MODE_BLEND_SHADOW,
	MODE_BLEND_MULTIPLY,
	MODE_BLEND_SUBTRACT,
	MODE_BLEND_INV_DESTRGB,
	MODE_BLEND_ALPHA,
	MODE_BLEND_ALPHA_INV,
};
void StgShotManager::Render(int targetPriority) {
	if (targetPriority < 0 || targetPriority >= listRenderQueueEnemy_.size()) return;

	const RenderQueue& renderQueuePlayer = listRenderQueuePlayer_[targetPriority];
	const RenderQueue& renderQueueEnemy = listRenderQueueEnemy_[targetPriority];
	if (renderQueuePlayer.count == 0 && renderQueueEnemy.count == 0) return;

	DirectGraphics* graphics = DirectGraphics::GetBase();
	IDirect3DDevice9* device = graphics->GetDevice();
	RenderShaderLibrary* shaderManager = ShaderManager::GetBase()->GetRenderLib();

	graphics->SetZBufferEnable(false);
	graphics->SetZWriteEnable(false);
	graphics->SetCullingMode(D3DCULL_NONE);
	graphics->SetLightingEnable(false);
	graphics->SetTextureFilter(filterMin_, filterMag_, D3DTEXF_NONE);

	DWORD bEnableFog = FALSE;
	device->GetRenderState(D3DRS_FOGENABLE, &bEnableFog);
	if (bEnableFog)
		graphics->SetFogEnable(false);

	auto& camera3D = graphics->GetCamera();
	auto& camera2D = graphics->GetCamera2D();

	D3DXMatrixMultiply(&matProj_, &camera2D->GetMatrix(), &graphics->GetViewPortMatrix());

	device->SetFVF(VERTEX_TLX::fvf);
	device->SetVertexDeclaration(shaderManager->GetVertexDeclarationTLX());

	if (D3DXHANDLE handle = effectShot_->GetParameterBySemantic(nullptr, "VIEWPROJECTION")) {
		effectShot_->SetMatrix(handle, &matProj_);
	}

	auto _RenderQueue = [&](const RenderQueue& renderQueue) {
		if (renderQueue.count == 0) return;

		for (size_t iBlend = 0; iBlend < blendTypeRenderOrder.size(); ++iBlend) {
			BlendMode blend = blendTypeRenderOrder[iBlend];

			graphics->SetBlendMode(blend);
			effectShot_->SetTechnique(blend == MODE_BLEND_ALPHA_INV ? "RenderInv" : "Render");

			for (size_t i = 0; i < renderQueue.count; ++i) {
				StgShotObject* pShot = renderQueue.listShot[i];
				pShot->Render(blend);
			}
		}
	};

	//Always renders enemy shots above player shots, completely obliterates TAΣ's wet dream.
	_RenderQueue(renderQueuePlayer);
	_RenderQueue(renderQueueEnemy);

	device->SetVertexShader(nullptr);
	device->SetPixelShader(nullptr);
	device->SetVertexDeclaration(nullptr);
	device->SetIndices(nullptr);

	if (bEnableFog)
		graphics->SetFogEnable(true);
}
void StgShotManager::LoadRenderQueue() {
	for (size_t i = 0; i < listRenderQueuePlayer_.size(); ++i) {
		listRenderQueuePlayer_[i].count = 0;
		listRenderQueueEnemy_[i].count = 0;
	}

	for (ref_unsync_ptr<StgShotObject>& obj : listObj_) {
		if (obj->IsDeleted() || !obj->IsActive() || !obj->IsVisible()) continue;

		auto& [count, listShot] = (obj->GetOwnerType() == StgShotObject::OWNER_PLAYER ?
			listRenderQueuePlayer_ : listRenderQueueEnemy_)[obj->GetRenderPriorityI()];

		while (count >= listShot.size())
			listShot.resize(listShot.size() * 2);
		listShot[count++] = obj.get();
	}
}

void StgShotManager::RegistIntersectionTarget() {
	for (ref_unsync_ptr<StgShotObject>& obj : listObj_) {
		if (!obj->IsDeleted() && obj->IsActive()) {
			obj->ClearIntersectedIdList();
			obj->RegistIntersectionTarget();
		}
	}
}
void StgShotManager::AddShot(ref_unsync_ptr<StgShotObject> obj) {
	obj->SetOwnObjectReference();
	listObj_.push_back(obj);
}

size_t StgShotManager::DeleteInCircle(int typeDelete, int typeTo, int typeOwner, int cx, int cy, optional<int> radius) {
	int r = radius.has_value() ? *radius : 0;
	int rr = r * r;

	DxRect<int> rcBox(cx - r, cy - r, cx + r, cy + r);

	size_t res = 0;

	for (ref_unsync_ptr<StgShotObject>& obj : listObj_) {
		if (obj->IsDeleted()) continue;
		if ((typeOwner != StgShotObject::OWNER_NULL) && (obj->GetOwnerType() != typeOwner)) continue;
		if (typeDelete == DEL_TYPE_SHOT && obj->IsSpellResist()) continue;
		if (obj->GetFadeDelete() >= 0) continue;

		int sx = obj->GetPositionX();
		int sy = obj->GetPositionY();

		bool bInRadius = rcBox.IsPointIntersected(sx, sy) && Math::HypotSq<int64_t>(cx - sx, cy - sy) <= rr;
		if (!radius.has_value() || bInRadius) {
			if (obj->GetObjectType() == TypeObject::Shot)
				++res;
			else {
				StgLaserObject* laser = dynamic_cast<StgLaserObject*>(obj.get());
				res += floor(laser->GetLength() / laser->GetItemDistance());
			}

			if (typeTo == TO_TYPE_IMMEDIATE)
				obj->DeleteImmediate();
			else if (typeTo == TO_TYPE_FADE)
				obj->SetFadeDelete();
			else if (typeTo == TO_TYPE_ITEM)
				obj->ConvertToItem();
		}
	}

	return res;
}
size_t StgShotManager::DeleteInRegularPolygon(int typeDelete, int typeTo, int typeOwner, int cx, int cy, int radius, int edges, double angle) {
	int r = radius;

	DxRect<int> rcBox(cx - r, cy - r, cx + r, cy + r);

	size_t res = 0;

	for (ref_unsync_ptr<StgShotObject>& obj : listObj_) {
		if (obj->IsDeleted()) continue;
		if ((typeOwner != StgShotObject::OWNER_NULL) && (obj->GetOwnerType() != typeOwner)) continue;
		if (typeDelete == DEL_TYPE_SHOT && obj->IsSpellResist()) continue;
		if (obj->GetFadeDelete() >= 0) continue;

		int sx = obj->GetPositionX();
		int sy = obj->GetPositionY();

		bool bInPolygon = false;
		if (rcBox.IsPointIntersected(sx, sy)) {
			float f = GM_PI / (float) edges;
			float cf = cosf(f);
			float dx = sx - cx;
			float dy = sy - cy;
			float dist = hypotf(dy, dx);
				
			bInPolygon = dist <= r;
			if (bInPolygon) {
				double r_apothem = r * cf;
				bInPolygon = dist <= r_apothem;
				if (!bInPolygon) {
					double ang = fmod(Math::NormalizeAngleRad(atan2(dy, dx)) - Math::DegreeToRadian(angle), 2 * f);
					bInPolygon = dist <= (r_apothem / cos(ang - f));
				}
			}
		}
		if (bInPolygon) {
			if (obj->GetObjectType() == TypeObject::Shot)
				++res;
			else {
				StgLaserObject* laser = dynamic_cast<StgLaserObject*>(obj.get());
				res += floor(laser->GetLength() / laser->GetItemDistance());
			}

			if (typeTo == TO_TYPE_IMMEDIATE)
				obj->DeleteImmediate();
			else if (typeTo == TO_TYPE_FADE)
				obj->SetFadeDelete();
			else if (typeTo == TO_TYPE_ITEM)
				obj->ConvertToItem();
		}
	}

	return res;
}

std::vector<int> StgShotManager::GetShotIdInCircle(int typeOwner, int cx, int cy, optional<int> radius) {
	std::vector<int> res;

	if (!radius.has_value()) {
		for (ref_unsync_ptr<StgShotObject>& obj : listObj_) {
			if (obj->IsDeleted()) continue;
			if ((typeOwner != StgShotObject::OWNER_NULL) && (obj->GetOwnerType() != typeOwner)) continue;
			if (obj->GetFadeDelete() >= 0) continue;

			res.push_back(obj->GetObjectID());
		}
	}
	else {
		int r = *radius;
		int rr = r * r;
		DxRect<int> rcBox(cx - r, cy - r, cx + r, cy + r);

		for (ref_unsync_ptr<StgShotObject>& obj : listObj_) {
			if (obj->IsDeleted()) continue;
			if ((typeOwner != StgShotObject::OWNER_NULL) && (obj->GetOwnerType() != typeOwner)) continue;
			if (obj->GetFadeDelete() >= 0) continue;

			int sx = obj->GetPositionX();
			int sy = obj->GetPositionY();
			bool bInRadius = rcBox.IsPointIntersected(sx, sy) && Math::HypotSq<int64_t>(cx - sx, cy - sy) <= rr;
			if (bInRadius)
				res.push_back(obj->GetObjectID());
		}
	}

	return res;
}
std::vector<int> StgShotManager::GetShotIdInRegularPolygon(int typeOwner, int cx, int cy, int radius, int edges, double angle) {
	int r = radius;

	DxRect<int> rcBox(cx - r, cy - r, cx + r, cy + r);

	std::vector<int> res;
	for (ref_unsync_ptr<StgShotObject>& obj : listObj_) {
		if (obj->IsDeleted()) continue;
		if ((typeOwner != StgShotObject::OWNER_NULL) && (obj->GetOwnerType() != typeOwner)) continue;
		if (obj->GetFadeDelete() >= 0) continue;

		int sx = obj->GetPositionX();
		int sy = obj->GetPositionY();

		bool bInPolygon = false;
		if (rcBox.IsPointIntersected(sx, sy)) {
			float f = GM_PI / (float)edges;
			float cf = cosf(f);
			float dx = sx - cx;
			float dy = sy - cy;
			float dist = hypotf(dy, dx);

			bInPolygon = dist <= r;
			if (bInPolygon) {
				double r_apothem = r * cf;
				bInPolygon = dist <= r_apothem;
				if (!bInPolygon) {
					double ang = fmod(Math::NormalizeAngleRad(atan2(dy, dx)) - Math::DegreeToRadian(angle), 2 * f);
					bInPolygon = dist <= (r_apothem / cos(ang - f));
				}
			}
		}
		if (bInPolygon)
			res.push_back(obj->GetObjectID());
	}

	return res;
}
std::vector<int> StgShotManager::GetLaserIdAll(int typeOwner) {
	std::vector<int> res;
	for (ref_unsync_ptr<StgShotObject>& obj : listObj_) {
		if (obj->IsDeleted()) continue;
		if ((typeOwner != StgShotObject::OWNER_NULL) && (obj->GetOwnerType() != typeOwner)) continue;
		if (obj->GetObjectType() == TypeObject::Shot) continue;
		if (obj->GetFadeDelete() >= 0) continue;

		res.push_back(obj->GetObjectID());
	}

	return res;
}
size_t StgShotManager::GetShotCount(int typeOwner) {
	size_t res = 0;

	for (ref_unsync_ptr<StgShotObject>& obj : listObj_) {
		if (obj->IsDeleted()) continue;
		if ((typeOwner != StgShotObject::OWNER_NULL) && (obj->GetOwnerType() != typeOwner)) continue;
		++res;
	}

	return res;
}

int StgShotManager::_TypeDeleteToEventType(TypeDelete type) {
	switch (type) {
	case TypeDelete::Immediate:
		return StgStageItemScript::EV_DELETE_SHOT_IMMEDIATE;
	case TypeDelete::Fade:
		return StgStageItemScript::EV_DELETE_SHOT_FADE;
	case TypeDelete::Item:
		return StgStageItemScript::EV_DELETE_SHOT_TO_ITEM;
	}
	return StgStageItemScript::EV_DELETE_SHOT_IMMEDIATE;
}
StgShotManager::TypeDelete StgShotManager::_EventTypeToTypeDelete(int type) {
	switch (type) {
	case StgStageItemScript::EV_DELETE_SHOT_IMMEDIATE:
		return TypeDelete::Immediate;
	case StgStageItemScript::EV_DELETE_SHOT_FADE:
		return TypeDelete::Fade;
	case StgStageItemScript::EV_DELETE_SHOT_TO_ITEM:
		return TypeDelete::Item;
	}
	return TypeDelete::Immediate;
}

void StgShotManager::SetDeleteEventEnableByType(int type, bool bEnable) {
	int bit = (int)_EventTypeToTypeDelete(type);
	listDeleteEventEnable_.set(bit, bEnable);
}
bool StgShotManager::LoadPlayerShotData(const std::wstring& path, bool bReload) {
	return listPlayerShotData_->AddShotDataList(path, bReload);
}
bool StgShotManager::LoadEnemyShotData(const std::wstring& path, bool bReload) {
	return listEnemyShotData_->AddShotDataList(path, bReload);
}

//****************************************************************************
//StgShotDataList
//****************************************************************************
StgShotDataList::StgShotDataList() {
	defaultDelayData_ = -1;
	defaultDelayColor_ = 0xffffffff;	//Solid white
}
StgShotDataList::~StgShotDataList() {
}
void StgShotDataList::_LoadVertexBuffers(std::map<std::wstring, VBContainerList>::iterator placement, 
	shared_ptr<Texture> texture, std::vector<StgShotData*>& listAddData)
{
	DirectGraphics* graphics = DirectGraphics::GetBase();
	IDirect3DDevice9* device = graphics->GetDevice();

	float texW = texture->GetWidth();
	float texH = texture->GetHeight();

	size_t countFrame = 0;
	for (StgShotData* iData : listAddData)
		countFrame += iData->GetFrameCount();

	size_t iBuffer = 0;
	while (countFrame > 0) {
		size_t thisCountFrame = std::min<size_t>(countFrame, StgShotVertexBufferContainer::MAX_DATA);

		placement->second.push_back(unique_ptr<StgShotVertexBufferContainer>(
			new StgShotVertexBufferContainer()));
		StgShotVertexBufferContainer* pVertexBufferContainer = placement->second.back().get();
		pVertexBufferContainer->SetTexture(texture);

		std::vector<VERTEX_TLX> bufferVertex(4 * thisCountFrame);
		size_t iVertex = 0;

		VERTEX_TLX verts[4];
		for (size_t iData = 0; iData < listAddData.size(); ++iData) {
			StgShotData* data = listAddData[iData];
			for (size_t iAnim = 0; iAnim < data->GetFrameCount(); ++iAnim) {
				StgShotDataFrame* pFrame = &data->listFrame_[iAnim];
				pFrame->listShotData_ = this;

				LONG* ptrSrc = reinterpret_cast<LONG*>(&pFrame->rcSrc_);
				float* ptrDst = reinterpret_cast<float*>(&pFrame->rcDst_);

				for (size_t iVert = 0; iVert < 4; ++iVert) {
					VERTEX_TLX* pv = &verts[iVert];

					//((iVert & 1) << 1)
					//   0 -> 0
					//   1 -> 2
					//   2 -> 0
					//   3 -> 2
					//(iVert | 1)
					//   0 -> 1
					//   1 -> 1
					//   2 -> 3
					//   3 -> 3

					StgShotObject::_SetVertexUV(pv,
						ptrSrc[(iVert & 1) << 1] / texW, ptrSrc[iVert | 1] / texH);
					StgShotObject::_SetVertexPosition(pv, ptrDst[(iVert & 1) << 1], ptrDst[iVert | 1], 0);
					StgShotObject::_SetVertexColorARGB(pv, 0xffffffff);
				}

				pFrame->pVertexBuffer_ = pVertexBufferContainer;
				pFrame->vertexOffset_ = iVertex;

				for (size_t j = 0; j < 4; ++j)
					bufferVertex[iVertex + j] = verts[j];
				iVertex += 4;
			}
		}

		HRESULT hr = pVertexBufferContainer->LoadData(bufferVertex, thisCountFrame);
		if (FAILED(hr)) {
			std::wstring err = StringUtility::Format(L"AddShotDataList::Failed to load shot data buffer: "
				"\t\r\n%s: %s",
				DXGetErrorString(hr), DXGetErrorDescription(hr));
			throw gstd::wexception(err);
		}

		++iBuffer;
		countFrame -= thisCountFrame;
	}
}
bool StgShotDataList::AddShotDataList(const std::wstring& path, bool bReload) {
	auto itrVB = mapVertexBuffer_.find(path);
	if (!bReload && itrVB != mapVertexBuffer_.end()) return true;

	std::wstring pathReduce = PathProperty::ReduceModuleDirectory(path);

	shared_ptr<FileReader> reader = FileManager::GetBase()->GetFileReader(path);
	if (reader == nullptr || !reader->Open())
		throw gstd::wexception(L"AddShotDataList: " + ErrorUtility::GetFileNotFoundErrorMessage(pathReduce, true));

	std::string source = reader->ReadAllString();

	bool res = false;
	Scanner scanner(source);
	try {
		std::map<int, unique_ptr<StgShotData>> mapData;
		std::wstring pathImage;

		while (scanner.HasNext()) {
			Token& tok = scanner.Next();
			if (tok.GetType() == Token::Type::TK_EOF)
				break;
			else if (tok.GetType() == Token::Type::TK_ID) {
				std::wstring element = tok.GetElement();
				if (element == L"ShotData") {
					_ScanShot(mapData, scanner);
				}
				else if (element == L"shot_image") {
					scanner.CheckType(scanner.Next(), Token::Type::TK_EQUAL);
					pathImage = scanner.Next().GetString();
				}
				else if (element == L"delay_id") {
					scanner.CheckType(scanner.Next(), Token::Type::TK_EQUAL);
					defaultDelayData_ = scanner.Next().GetInteger();
				}
				else if (element == L"delay_color") {
					std::vector<std::wstring> list = scanner.GetArgumentList();

					if (list.size() < 3)
						throw wexception("Invalid argument list size (expected 3)");

					defaultDelayColor_ = D3DCOLOR_ARGB(255,
						StringUtility::ToInteger(list[0]),
						StringUtility::ToInteger(list[1]),
						StringUtility::ToInteger(list[2]));
				}
				
				if (scanner.HasNext())
					tok = scanner.Next();
			}
		}

		if (pathImage.size() == 0) throw gstd::wexception("Shot texture must be set.");
		std::wstring dir = PathProperty::GetFileDirectory(path);
		pathImage = StringUtility::Replace(pathImage, L"./", dir);
		pathImage = PathProperty::GetUnique(pathImage);

		shared_ptr<Texture> texture;
		{
			TextureManager* textureManager = TextureManager::GetBase();

			if ((texture = textureManager->GetTexture(pathImage)) == nullptr) {
				texture = std::make_shared<Texture>();
				if (!texture->CreateFromFile(pathImage, false, false))
					texture = nullptr;
			}
		}
		if (texture == nullptr) {
			throw gstd::wexception("Failed to load the specified shot texture.");
		}

		std::vector<StgShotData*> listAddData;

		size_t countFrame = 0;
		{
			size_t i = 0;
			for (auto itr = mapData.begin(); itr != mapData.end(); ++itr, ++i) {
				int id = itr->first;
				unique_ptr<StgShotData>& data = itr->second;
				if (data == nullptr) continue;

				for (auto& iFrame : data->listFrame_)
					iFrame.listShotData_ = this;
				countFrame += data->GetFrameCount();

				listAddData.push_back(data.get());
				if (listData_.size() <= id)
					listData_.resize(id + 1);
				listData_[id] = std::move(data);		//Moves unique_ptr object, do not use mapData after this point
			}
		}

		if (itrVB != mapVertexBuffer_.end()) {
			itrVB->second.clear();
			_LoadVertexBuffers(itrVB, texture, listAddData);
		}
		else {
			itrVB = mapVertexBuffer_.insert({ path, VBContainerList() }).first;
			_LoadVertexBuffers(itrVB, texture, listAddData);
		}

		Logger::WriteTop(StringUtility::Format(L"Loaded shot data: %s", pathReduce.c_str()));
		res = true;
	}
	catch (gstd::wexception& e) {
		std::wstring log = StringUtility::Format(L"Failed to load shot data: %s\r\n\t[Line=%d] (%s)",
			pathReduce.c_str(), scanner.GetCurrentLine(), e.what());
		Logger::WriteTop(log);
		res = false;
	}
	catch (...) {
		std::string log = StringUtility::Format("Failed to load shot data: %s\r\n\t[Line=%d] (Unknown error.)",
			pathReduce.c_str(), scanner.GetCurrentLine());
		Logger::WriteTop(log);
		res = false;
	}

	return res;
}
void StgShotDataList::_ScanShot(std::map<int, unique_ptr<StgShotData>>& mapData, Scanner& scanner) {
	Token& tok = scanner.Next();
	if (tok.GetType() == Token::Type::TK_NEWLINE) tok = scanner.Next();
	scanner.CheckType(tok, Token::Type::TK_OPENC);

	struct Data {
		StgShotData* shotData;
		int id = -1;
	} data;
	data.shotData = new StgShotData(this);
	data.shotData->idDefaultDelay_ = defaultDelayData_;
	data.shotData->colorDelay_ = defaultDelayColor_;

	//--------------------------------------------------------------

#define LAMBDA_SETV(m, f) [](Data* i, Scanner& s) { \
		s.CheckType(s.Next(), Token::Type::TK_EQUAL); \
		i->m = s.Next().f(); \
	}
	auto funcSetRect = [](Data* i, Scanner& s) {
		std::vector<std::wstring> list = s.GetArgumentList();

		if (list.size() < 4)
			throw wexception("Invalid argument list size (expected 4)");

		DxRect<LONG> rect(StringUtility::ToInteger(list[0]), StringUtility::ToInteger(list[1]),
			StringUtility::ToInteger(list[2]), StringUtility::ToInteger(list[3]));

		StgShotDataFrame dFrame;
		dFrame.rcSrc_ = rect;
		dFrame.rcDst_ = StgShotDataFrame::LoadDestRect(&rect);

		i->shotData->listFrame_ = { dFrame };
		i->shotData->totalFrame_ = 1;
	};
	auto funcSetDelayID = [](Data* i, Scanner& s) {
		s.CheckType(s.Next(), Token::Type::TK_EQUAL);
		i->shotData->idDefaultDelay_ = s.Next().GetInteger();
	};
	auto funcSetDelayColor = [](Data* i, Scanner& s) {
		std::vector<std::wstring> list = s.GetArgumentList();

		if (list.size() < 3)
			throw wexception("Invalid argument list size (expected 3)");

		i->shotData->colorDelay_ = D3DCOLOR_ARGB(255,
			StringUtility::ToInteger(list[0]),
			StringUtility::ToInteger(list[1]),
			StringUtility::ToInteger(list[2]));
	};

	static const std::unordered_map<std::wstring, BlendMode> mapBlendType = {
		{ L"ADD", MODE_BLEND_ADD_RGB },
		{ L"ADD_RGB", MODE_BLEND_ADD_RGB },
		{ L"ADD_ARGB", MODE_BLEND_ADD_ARGB },
		{ L"MULTIPLY", MODE_BLEND_MULTIPLY },
		{ L"SUBTRACT", MODE_BLEND_SUBTRACT },
		{ L"SHADOW", MODE_BLEND_SHADOW },
		{ L"ALPHA_INV", MODE_BLEND_ALPHA_INV },
	};
#define LAMBDA_SETBLEND(m) [](Data* i, Scanner& s) { \
		s.CheckType(s.Next(), Token::Type::TK_EQUAL); \
		BlendMode typeRender = MODE_BLEND_ALPHA; \
		auto itr = mapBlendType.find(s.Next().GetElement()); \
		if (itr != mapBlendType.end()) \
			typeRender = itr->second; \
		i->shotData->m = typeRender; \
	}
	auto funcSetCollision = [](Data* i, Scanner& s) {
		DxCircle circle;
		std::vector<std::wstring> list = s.GetArgumentList();
		if (list.size() == 1) {
			circle.SetR(StringUtility::ToDouble(list[0]));
		}
		else if (list.size() == 3) {
			circle.SetR(StringUtility::ToDouble(list[0]));
			circle.SetX(StringUtility::ToDouble(list[1]));
			circle.SetY(StringUtility::ToDouble(list[2]));
		}
		i->shotData->listCol_.push_back(circle);
	};
	auto funcSetAngularVel = [](Data* i, Scanner& s) {
		s.CheckType(s.Next(), Token::Type::TK_EQUAL);
		Token& tok = s.Next();
		if (tok.GetElement() == L"rand") {
			std::vector<std::wstring> list = s.GetArgumentList(false);
			if (list.size() == 2) {
				i->shotData->angularVelocityMin_ = Math::DegreeToRadian(StringUtility::ToDouble(list[0]));
				i->shotData->angularVelocityMax_ = Math::DegreeToRadian(StringUtility::ToDouble(list[1]));
			}
		}
		else {
			i->shotData->angularVelocityMin_ = Math::DegreeToRadian(tok.GetReal());
			i->shotData->angularVelocityMax_ = i->shotData->angularVelocityMin_;
		}
	};
	auto funcLoadAnimation = [](Data* i, Scanner& s) {
		i->shotData->listFrame_.clear();
		i->shotData->totalFrame_ = 0;
		_ScanAnimation(i->shotData, s);
	};

	//Do NOT use [&] lambdas
	static const std::unordered_map<std::wstring, std::function<void(Data*, Scanner&)>> mapFunc = {
		{ L"id", LAMBDA_SETV(id, GetInteger) },
		{ L"rect", funcSetRect },
		{ L"delay_id", funcSetDelayID },
		{ L"delay_color", funcSetDelayColor },
		{ L"collision", funcSetCollision },
		{ L"render", LAMBDA_SETBLEND(typeRender_) },
		{ L"delay_render", LAMBDA_SETBLEND(typeDelayRender_) },
		{ L"alpha", LAMBDA_SETV(shotData->alpha_, GetInteger) },
		{ L"angular_velocity", funcSetAngularVel },
		{ L"fixed_angle", LAMBDA_SETV(shotData->bFixedAngle_, GetBoolean) },
		{ L"AnimationData", funcLoadAnimation },
	};

#undef LAMBDA_SETV
#undef LAMBDA_CREATELIST
#undef LAMBDA_SETBLEND

	//--------------------------------------------------------------

	while (true) {
		tok = scanner.Next();
		if (tok.GetType() == Token::Type::TK_CLOSEC) {
			break;
		}
		else if (tok.GetType() == Token::Type::TK_ID) {
			std::wstring element = tok.GetElement();

			auto itrFind = mapFunc.find(element);
			if (itrFind != mapFunc.end())
				itrFind->second(&data, scanner);
		}
	}

	if (data.id >= 0) {
		if (data.shotData->listCol_.size() == 0) {
			float r = 0;
			if (data.shotData->listFrame_.size() > 0) {
				DxRect<LONG>& rect = data.shotData->listFrame_[0].rcSrc_;
				r = std::min(abs(rect.GetWidth()), abs(rect.GetHeight())) / 3.0f - 3.0f;
			}
			data.shotData->listCol_.push_back(DxCircle(0, 0, r));
		}

		mapData[data.id] = unique_ptr<StgShotData>(data.shotData);
	}
}
void StgShotDataList::_ScanAnimation(StgShotData* shotData, Scanner& scanner) {
	Token& tok = scanner.Next();
	if (tok.GetType() == Token::Type::TK_NEWLINE) tok = scanner.Next();
	scanner.CheckType(tok, Token::Type::TK_OPENC);

	while (true) {
		tok = scanner.Next();
		if (tok.GetType() == Token::Type::TK_CLOSEC) {
			break;
		}
		else if (tok.GetType() == Token::Type::TK_ID) {
			std::wstring element = tok.GetElement();

			if (element == L"animation_data") {
				std::vector<std::wstring> list = scanner.GetArgumentList();

				if (list.size() < 5)
					throw wexception("Invalid argument list size (expected 5)");

				int frame = StringUtility::ToInteger(list[0]);
				DxRect<LONG> rect(StringUtility::ToInteger(list[1]), StringUtility::ToInteger(list[2]),
					StringUtility::ToInteger(list[3]), StringUtility::ToInteger(list[4]));

				StgShotDataFrame dFrame;
				dFrame.frame_ = frame;
				dFrame.rcSrc_ = rect;
				dFrame.rcDst_ = StgShotDataFrame::LoadDestRect(&rect);

				shotData->listFrame_.push_back(dFrame);
				shotData->totalFrame_ += frame;
			}
		}
	}
}

//*******************************************************************
//StgShotDataFrame
//*******************************************************************
StgShotDataFrame::StgShotDataFrame() {
	listShotData_ = nullptr;
	pVertexBuffer_ = nullptr;
	vertexOffset_ = 0;
	frame_ = 0;
}
DxRect<float> StgShotDataFrame::LoadDestRect(DxRect<LONG>* src) {
	float width = src->GetWidth() / 2.0f;
	float height = src->GetHeight() / 2.0f;
	return DxRect<float>(-width, -height, width, height);
}

//*******************************************************************
//StgShotData
//*******************************************************************
StgShotData::StgShotData(StgShotDataList* listShotData) {
	listShotData_ = listShotData;

	typeRender_ = MODE_BLEND_ALPHA;
	typeDelayRender_ = MODE_BLEND_ADD_ARGB;

	alpha_ = 255;

	idDefaultDelay_ = -1;
	colorDelay_ = D3DCOLOR_ARGB(255, 255, 255, 255);

	totalFrame_ = 0;

	angularVelocityMin_ = 0;
	angularVelocityMax_ = 0;
	bFixedAngle_ = false;
}
StgShotData::~StgShotData() {
}

StgShotDataFrame* StgShotData::GetFrame(size_t frame) {
	if (totalFrame_ <= 1U)
		return &listFrame_[0];

	frame = frame % totalFrame_;
	size_t total = 0;

	for (auto itr = listFrame_.begin(); itr != listFrame_.end(); ++itr) {
		total += itr->frame_;
		if (total >= frame)
			return &(*itr);
	}
	return &listFrame_[0];
}

//****************************************************************************
//StgShotVertexBufferContainer
//****************************************************************************
StgShotVertexBufferContainer::StgShotVertexBufferContainer() {
	DirectGraphics* graphics = DirectGraphics::GetBase();
	VertexBufferManager* vbManager = VertexBufferManager::GetBase();

	pVertexBuffer_ = (FixedVertexBuffer*)vbManager->CreateExtraVertexBuffer();

	countData_ = 0;
}
StgShotVertexBufferContainer::~StgShotVertexBufferContainer() {
	VertexBufferManager* vbManager = VertexBufferManager::GetBase();
	vbManager->ReleaseExtraVertexBuffer((size_t)pVertexBuffer_);
}

HRESULT StgShotVertexBufferContainer::LoadData(const std::vector<VERTEX_TLX>& data, size_t countFrame) {
	pVertexBuffer_->Setup(data.size(), StgShotVertexBufferContainer::STRIDE, VERTEX_TLX::fvf);

	HRESULT hr = pVertexBuffer_->Create(0, D3DPOOL_MANAGED);
	if (FAILED(hr)) {
		return hr;
	}

	BufferLockParameter lockParam = BufferLockParameter(D3DLOCK_DISCARD);
	lockParam.SetSource(const_cast<std::vector<VERTEX_TLX>&>(data), pVertexBuffer_->GetSize(), sizeof(VERTEX_TLX));

	hr = pVertexBuffer_->UpdateBuffer(&lockParam);
	countData_ = countFrame;

	return hr;
}

//****************************************************************************
//StgShotObject
//****************************************************************************
StgShotObject::StgShotObject(StgStageController* stageController) : StgMoveObject(stageController) {
	frameWork_ = 0;
	posX_ = 0;
	posY_ = 0;
	idShotData_ = 0;
	SetBlendType(MODE_BLEND_NONE);

	bRequestedPlayerDeleteEvent_ = false;
	damage_ = 1;
	life_ = 1;

	bAutoDelete_ = true;
	bEraseShot_ = false;
	bSpellFactor_ = false;
	bSpellResist_ = false;

	color_ = D3DCOLOR_ARGB(255, 255, 255, 255);

	frameGrazeInvalid_ = 0;
	frameGrazeInvalidStart_ = -1;

	bPenetrateShot_ = true;
	frameEnemyHitInvalid_ = 0;

	frameFadeDelete_ = -1;
	frameAutoDelete_ = INT_MAX;
	typeAutoDelete_ = StgShotManager::TO_TYPE_IMMEDIATE;

	typeOwner_ = OWNER_ENEMY;

	bUserIntersectionMode_ = false;
	bIntersectionEnable_ = true;
	bChangeItemEnable_ = true;

	bEnableMotionDelay_ = false;
	bRoundingPosition_ = false;
	roundingAngle_ = 0;

	hitboxScale_ = D3DXVECTOR2(1.0f, 1.0f);

	int priShotI = stageController_->GetStageInformation()->GetShotObjectPriority();
	SetRenderPriorityI(priShotI);
}
StgShotObject::~StgShotObject() {
}

void StgShotObject::Clone(DxScriptObjectBase* _src, bool deepCopy) {
	DxScriptShaderObject::Clone(_src, deepCopy);

	auto src = (StgShotObject*)_src;
	StgMoveObject::Copy((StgMoveObject*)src);
	StgIntersectionObject::Copy((StgIntersectionObject*)src);

	frameWork_ = src->frameWork_;
	idShotData_ = src->idShotData_;
	typeOwner_ = src->typeOwner_;

	move_ = src->move_;
	lastAngle_ = src->lastAngle_;

	hitboxScale_ = src->hitboxScale_;
	delay_ = src->delay_;
	anim_ = src->anim_;

	frameGrazeInvalid_ = src->frameGrazeInvalid_;
	frameGrazeInvalidStart_ = src->frameGrazeInvalidStart_;
	frameFadeDelete_ = src->frameFadeDelete_;
	bPenetrateShot_ = src->bPenetrateShot_;

	renderTarget_ = src->renderTarget_;

	frameEnemyHitInvalid_ = src->frameEnemyHitInvalid_;
	mapEnemyHitCooldown_ = src->mapEnemyHitCooldown_;

	bRequestedPlayerDeleteEvent_ = src->bRequestedPlayerDeleteEvent_;
	damage_ = src->damage_;
	life_ = src->life_;

	bAutoDelete_ = src->bAutoDelete_;
	bEraseShot_ = src->bEraseShot_;
	bSpellFactor_ = src->bSpellFactor_;
	bSpellResist_ = src->bSpellResist_;
	frameAutoDelete_ = src->frameAutoDelete_;
	typeAutoDelete_ = src->typeAutoDelete_;

	listIntersectionTarget_ = src->listIntersectionTarget_;
	bUserIntersectionMode_ = src->bUserIntersectionMode_;
	bIntersectionEnable_ = src->bIntersectionEnable_;
	bChangeItemEnable_ = src->bChangeItemEnable_;

	bEnableMotionDelay_ = src->bEnableMotionDelay_;
	bRoundingPosition_ = src->bRoundingPosition_;
	roundingAngle_ = src->roundingAngle_;
}

void StgShotObject::SetOwnObjectReference() {
	auto ptr = ref_unsync_ptr<StgShotObject>::Cast(stageController_->GetMainRenderObject(idObject_));
	pOwnReference_ = ptr;
}
void StgShotObject::Work() {
}
void StgShotObject::_Move() {
	if (delay_.time == 0 || bEnableMotionDelay_)
		StgMoveObject::_Move();
	SetX(posX_);
	SetY(posY_);
}
void StgShotObject::_DeleteInLife() {
	if (IsDeleted() || life_ > 0) return;

	_SendDeleteEvent(TypeDelete::Immediate);
	_RequestPlayerDeleteEvent(DxScript::ID_INVALID);

	auto objectManager = stageController_->GetMainObjectManager();
	objectManager->DeleteObject(this);
}
void StgShotObject::_RequestPlayerDeleteEvent(int hitObjectID) {	//A super ugly hack, but it'll do for now
	if (bRequestedPlayerDeleteEvent_) return;
	bRequestedPlayerDeleteEvent_ = true;

	auto objectManager = stageController_->GetMainObjectManager();
	auto scriptManager = stageController_->GetScriptManager();

	if (scriptManager != nullptr && typeOwner_ == StgShotObject::OWNER_PLAYER) {
		float posX = GetPositionX();
		float posY = GetPositionY();
		LOCK_WEAK(scriptPlayer, scriptManager->GetPlayerScript()) {
			float listPos[2] = { posX, posY };

			value listScriptValue[4];
			listScriptValue[0] = scriptPlayer->CreateIntValue(idObject_);
			listScriptValue[1] = scriptPlayer->CreateFloatArrayValue(listPos, 2U);
			listScriptValue[2] = scriptPlayer->CreateIntValue(GetShotDataID());
			listScriptValue[3] = scriptPlayer->CreateIntValue(hitObjectID);
			scriptPlayer->RequestEvent(StgStagePlayerScript::EV_DELETE_SHOT_PLAYER, listScriptValue, 4);
		}
	}
}

void StgShotObject::_DeleteInAutoClip() {
	if (IsDeleted() || !IsAutoDelete()) return;

	DxRect<LONG>* const rcStgFrame = stageController_->GetStageInformation()->GetStgFrameRect();
	DxRect<LONG>* const rcClipBase = stageController_->GetShotManager()->GetShotDeleteClip();
	DxRect<LONG> rcDeleteClip(rcClipBase->left, rcClipBase->top,
		rcStgFrame->GetWidth() + rcClipBase->right,
		rcStgFrame->GetHeight() + rcClipBase->bottom);

	if (!rcDeleteClip.IsPointIntersected(posX_, posY_)) {
		auto objectManager = stageController_->GetMainObjectManager();
		objectManager->DeleteObject(this);
	}
}
void StgShotObject::_DeleteInFadeDelete() {
	if (IsDeleted()) return;
	if (frameFadeDelete_ == 0) {
		_SendDeleteEvent(TypeDelete::Fade);

		auto objectManager = stageController_->GetMainObjectManager();
		objectManager->DeleteObject(this);
	}
}
void StgShotObject::_DeleteInAutoDeleteFrame() {
	if (IsDeleted() || delay_.time > 0) return;

	if (frameAutoDelete_ <= 0) {
		switch (typeAutoDelete_) {
		case StgShotManager::TO_TYPE_IMMEDIATE:
			DeleteImmediate();
			break;
		case StgShotManager::TO_TYPE_FADE:
			SetFadeDelete();
			break;
		case StgShotManager::TO_TYPE_ITEM:
			ConvertToItem();
			break;
		}
	}
	else --frameAutoDelete_;
}
void StgShotObject::_CommonWorkTask() {
	if (bEnableMovement_) {
		++frameWork_;
		_DeleteInLife();
		_DeleteInAutoClip();
		_DeleteInAutoDeleteFrame();
	}
	if (frameFadeDelete_ >= 0) --frameFadeDelete_;
	_DeleteInFadeDelete();

	--frameGrazeInvalid_;

	//----------------------------------------------------------

	for (auto itr = mapEnemyHitCooldown_.begin(); itr != mapEnemyHitCooldown_.end();) {
		if (itr->first.expired() || itr->first->IsDeleted() || (--(itr->second) == 0))
			itr = mapEnemyHitCooldown_.erase(itr);
		else ++itr;
	}
}

bool StgShotObject::CheckEnemyHitCooldownExists(ref_unsync_weak_ptr<StgEnemyObject> obj) {
	if (mapEnemyHitCooldown_.empty()) return false;
	return mapEnemyHitCooldown_.find(obj) != mapEnemyHitCooldown_.end();
}
void StgShotObject::AddEnemyHitCooldown(ref_unsync_weak_ptr<StgEnemyObject> obj, uint32_t time) {
	if (obj) {
		mapEnemyHitCooldown_[obj] = time;
	}
}

void StgShotObject::Intersect(StgIntersectionTarget* ownTarget, StgIntersectionTarget* otherTarget) {
	ref_unsync_weak_ptr<StgIntersectionObject> obj = otherTarget->GetObject();

	StgIntersectionTarget::Type otherType = otherTarget->GetTargetType();
	switch (otherType) {
	case StgIntersectionTarget::TYPE_PLAYER:
	{
		if (frameGrazeInvalid_ <= 0)
			frameGrazeInvalid_ = frameGrazeInvalidStart_ > 0 ?
			frameGrazeInvalidStart_ : INT_MAX;
		break;
	}
	case StgIntersectionTarget::TYPE_PLAYER_SHOT:
	{
		if (obj) {
			if (StgShotObject* shot = dynamic_cast<StgShotObject*>(obj.get())) {
				bool bEraseShot = shot->IsEraseShot();
				if (bEraseShot && !bSpellResist_)
					ConvertToItem();
			}
		}
		break;
	}
	case StgIntersectionTarget::TYPE_PLAYER_SPELL:
	{
		if (obj) {
			if (StgPlayerSpellObject* spell = dynamic_cast<StgPlayerSpellObject*>(obj.get())) {
				bool bEraseShot = spell->IsEraseShot();
				if (bEraseShot && !bSpellResist_)
					ConvertToItem();
			}
		}
		break;
	}
	case StgIntersectionTarget::TYPE_ENEMY:
	{
		if (!bSpellResist_) {
			//Register intersection only if the enemy is off hit cooldown
			if (!CheckEnemyHitCooldownExists(ref_unsync_weak_ptr<StgEnemyObject>::Cast(obj)))
				--life_;
		}
		break;
	}
	case StgIntersectionTarget::TYPE_ENEMY_SHOT:
	{
		if (!bSpellResist_ && bPenetrateShot_)
			--life_;
		break;
	}
	}

	if (life_ == 0)
		_RequestPlayerDeleteEvent(obj.IsExists() ? obj->GetDxScriptObjectID() : DxScript::ID_INVALID);
}
StgShotData* StgShotObject::_GetShotData(int id) {
	StgShotManager* shotManager = stageController_->GetShotManager();
	StgShotDataList* dataList = (typeOwner_ == OWNER_PLAYER) ?
		shotManager->GetPlayerShotDataList() : shotManager->GetEnemyShotDataList();
	return dataList ? dataList->GetData(id) : nullptr;
}

void StgShotObject::_SetVertexPosition(VERTEX_TLX* vertex, float x, float y, float z, float w) {
	constexpr float bias = 0.0f;

	x *= DirectGraphics::g_dxCoordsMul_;
	y *= DirectGraphics::g_dxCoordsMul_;

	vertex->position.x = x + bias;
	vertex->position.y = y + bias;
	vertex->position.z = z;
	vertex->position.w = w;
}
void StgShotObject::_SetVertexUV(VERTEX_TLX* vertex, float u, float v) {
	vertex->texcoord.x = u;
	vertex->texcoord.y = v;
}
void StgShotObject::_SetVertexColorARGB(VERTEX_TLX* vertex, D3DCOLOR color) {
	vertex->diffuse_color = color;
}
void StgShotObject::SetAlpha(int alpha) {
	ColorAccess::ClampColor(alpha);
	color_ = (color_ & 0x00ffffff) | ((byte)alpha << 24);
}
void StgShotObject::SetColor(int r, int g, int b) {
	__m128i c = Vectorize::Set(color_ >> 24, r, g, b);
	color_ = ColorAccess::ToD3DCOLOR(ColorAccess::ClampColorPacked(c));
}

void StgShotObject::ConvertToItem() {
	if (IsDeleted()) return;

	_SendDeleteEvent(bChangeItemEnable_ ? TypeDelete::Item : TypeDelete::Immediate);

	auto objectManager = stageController_->GetMainObjectManager();
	objectManager->DeleteObject(this);
}
void StgShotObject::DeleteImmediate() {
	if (IsDeleted()) return;

	_SendDeleteEvent(TypeDelete::Immediate);

	auto objectManager = stageController_->GetMainObjectManager();
	objectManager->DeleteObject(this);
}

//StgShotObject::DelayParameter
float StgShotObject::DelayParameter::_CalculateValue(D3DXVECTOR3* param, lerp_func func) {
	switch (type) {
	case DELAY_LERP:
		return func(param->x, param->y, time / param->z);
	case DELAY_DEFAULT:
	default:
		return std::min(param->x + time / param->z, param->y);
	}
}

//****************************************************************************
//StgNormalShotObject
//****************************************************************************
StgNormalShotObject::StgNormalShotObject(StgStageController* stageController) : StgShotObject(stageController) {
	typeObject_ = TypeObject::Shot;
	angularVelocity_ = 0;
	bFixedAngle_ = false;

	move_ = D3DXVECTOR2(1, 0);
	lastAngle_ = 0;
}
StgNormalShotObject::~StgNormalShotObject() {
}

void StgNormalShotObject::Clone(DxScriptObjectBase* _src, bool deepCopy) {
	StgShotObject::Clone(_src, deepCopy);

	auto src = (StgNormalShotObject*)_src;

	angularVelocity_ = src->angularVelocity_;
	bFixedAngle_ = src->bFixedAngle_;
}

void StgNormalShotObject::Work() {
	if (bEnableMovement_) {
		_Move();

		if (delay_.time > 0) {
			--(delay_.time);
			delay_.angle.x += delay_.angle.y;
		}

		{
			angle_.z += angularVelocity_;
			bool bDelay = (delay_.time > 0 && delay_.angle.y != 0);
			double angleZ = angle_.z + (bDelay ? delay_.angle.x : 0);
			if (StgShotData* shotData = _GetShotData()) {
				if (!bFixedAngle_ && !bDelay) angleZ += GetDirectionAngle() + Math::DegreeToRadian(90);
			}

			if (angleZ != lastAngle_) {
				double ang = (roundingAngle_ > 0) ? round(angleZ / roundingAngle_) * roundingAngle_ : angleZ;
				move_ = D3DXVECTOR2(cosf(ang), sinf(ang));
				lastAngle_ = angleZ;
			}
		}

		if (anim_.type) {
			switch (anim_.type) {
			case (AnimParameter::SHOTANIM_JIGGLE):
				// args -> total time, speed, scale amount, _
				anim_.time--;
				if (anim_.time <= 0)
					anim_.type = AnimParameter::SHOTANIM_NONE;
				else
					anim_.accumulate += anim_.time * anim_.args[1] / anim_.args[0];
				break;
			case (AnimParameter::SHOTANIM_FLUTTER):
				// args -> speed, move amount, scale offset, scale amount
				anim_.accumulate += anim_.args[0] + GetSpeed() * anim_.args[1];
				break;
			}
		}
	}

	_CommonWorkTask();
}

void StgNormalShotObject::_AddIntersectionRelativeTarget() {
	StgIntersectionManager* intersectionManager = stageController_->GetIntersectionManager();

	if ((IsDeleted() || delay_.time > 0 || frameFadeDelete_ >= 0)
		|| (bUserIntersectionMode_ || !bIntersectionEnable_)
		|| pOwnReference_.expired())
		return;

	StgShotData* shotData = _GetShotData();
	if (shotData == nullptr)
		return;

	ClearIntersected();
	bool res = GetIntersectionTargetList_NoVector(shotData);
	if (res) {
		for (auto& iTarget : listIntersectionTarget_) {
			if (iTarget.first && iTarget.second != nullptr)
				intersectionManager->AddTarget(iTarget.second);
		}
	}
}
StgIntersectionObject::IntersectionListType StgNormalShotObject::GetIntersectionTargetList() {
	if ((IsDeleted() || delay_.time > 0 || frameFadeDelete_ >= 0)
		|| (bUserIntersectionMode_ || !bIntersectionEnable_)
		|| pOwnReference_.expired())
		return IntersectionListType();

	StgShotData* shotData = _GetShotData();
	if (shotData == nullptr)
		return IntersectionListType();

	bool res = GetIntersectionTargetList_NoVector(shotData);
	if (res) return listIntersectionTarget_;

	return IntersectionListType();
}
bool StgNormalShotObject::GetIntersectionTargetList_NoVector(StgShotData* shotData) {
	float intersectionScale = (hitboxScale_.x + hitboxScale_.y) / 2.0f;
	if (abs(intersectionScale) < 0.01f)
		return false;

	auto& listCircle = shotData->GetIntersectionCircleList();
	if (listIntersectionTarget_.size() < listCircle.size())
		listIntersectionTarget_.resize(listCircle.size(), CreateEmptyIntersection());
	for (auto& i : listIntersectionTarget_) i.first = false;

	for (size_t i = 0; i < listCircle.size(); ++i) {
		IntersectionPairType* pPair = &listIntersectionTarget_[i];

		StgIntersectionTarget_Circle* pTarget = (StgIntersectionTarget_Circle*)(pPair->second.get());
		if (pTarget == nullptr) {
			pTarget = new StgIntersectionTarget_Circle();
			pPair->second.reset(pTarget);
		}

		const DxCircle* pSrcCircle = &listCircle[i];
		DxCircle* pDstCircle = &pTarget->GetCircle();
		if (pSrcCircle->GetR() <= 0)
			continue;
		pPair->first = true;

		if (pSrcCircle->GetX() != 0 || pSrcCircle->GetY() != 0) {
			float px = pSrcCircle->GetX() * move_.x + pSrcCircle->GetY() * move_.y;
			float py = pSrcCircle->GetX() * move_.y - pSrcCircle->GetY() * move_.x;
			pDstCircle->SetX(posX_ + px * intersectionScale);
			pDstCircle->SetY(posY_ + py * intersectionScale);
		}
		else {
			pDstCircle->SetX(posX_);
			pDstCircle->SetY(posY_);
		}
		pDstCircle->SetR(pSrcCircle->GetR() * intersectionScale);

		pTarget->SetTargetType(typeOwner_ == OWNER_PLAYER ?
			StgIntersectionTarget::TYPE_PLAYER_SHOT : StgIntersectionTarget::TYPE_ENEMY_SHOT);
		pTarget->SetObject(pOwnReference_);
		pTarget->SetIntersectionSpace();
	}

	return true;
}

void StgShotObject::_DefaultShotRender(StgShotData* shotData, StgShotDataFrame* shotFrame, const D3DXMATRIX& matWorld, D3DCOLOR color) {
	if (shotFrame == nullptr) return;
	StgShotManager* shotManager = stageController_->GetShotManager();

	StgShotVertexBufferContainer* pVB = shotFrame->GetVertexBufferContainer();
	DWORD vertexOffset = shotFrame->vertexOffset_;

	if (pVB) {
		DirectGraphics* graphics = DirectGraphics::GetBase();
		IDirect3DDevice9* device = graphics->GetDevice();

		if (graphics->IsAllowRenderTargetChange()) {
			if (auto pRT = renderTarget_.lock())
				graphics->SetRenderTarget(pRT);
			else graphics->SetRenderTarget(nullptr);
		}

		graphics->SetCurrentTexture(pVB->GetD3DTexture());
		device->SetStreamSource(0, pVB->GetD3DBuffer(), vertexOffset * sizeof(VERTEX_TLX), sizeof(VERTEX_TLX));

		{
			ID3DXEffect* effect = shotManager->GetEffect();
			if (shader_) {
				effect = shader_->GetEffect();
				if (shader_->LoadTechnique()) {
					shader_->LoadParameter();
				}
			}

			if (effect) {
				D3DXHANDLE handle = nullptr;
				if (handle = effect->GetParameterBySemantic(nullptr, "WORLD")) {
					effect->SetMatrix(handle, &matWorld);
				}
				if (shader_) {
					if (handle = effect->GetParameterBySemantic(nullptr, "VIEWPROJECTION")) {
						effect->SetMatrix(handle, shotManager->GetProjectionMatrix());
					}
				}
				if (handle = effect->GetParameterBySemantic(nullptr, "ICOLOR")) {
					//To normalized RGBA vector
					D3DXVECTOR4 vColor = ColorAccess::ToVec4Normalized(color, ColorAccess::PERMUTE_RGBA);
					effect->SetVector(handle, &vColor);
				}

				UINT countPass = 1;
				effect->Begin(&countPass, D3DXFX_DONOTSAVESHADERSTATE);
				for (UINT iPass = 0; iPass < countPass; ++iPass) {
					effect->BeginPass(iPass);
					device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
					effect->EndPass();
				}
				effect->End();
			}
		}
	}
}

void StgNormalShotObject::Render(BlendMode targetBlend) {
	//if (!IsVisible()) return;
	StgShotManager* shotManager = stageController_->GetShotManager();

	StgShotData* shotData = _GetShotData();
	if (shotData == nullptr) return;

	FLOAT sposx = position_.x;
	FLOAT sposy = position_.y;
	if (bRoundingPosition_) {
		sposx = roundf(sposx);
		sposy = roundf(sposy);
	}

	float scaleX = 1.0f;
	float scaleY = 1.0f;
	D3DCOLOR color;

	FLOAT rotX = move_.x;
	FLOAT rotY = move_.y;

	auto _Render = [&](StgShotData* pData, StgShotDataFrame* pFrame) {
		if (pData == nullptr || pFrame == nullptr) return;

		D3DXMATRIX matTransform(
			scaleX * rotX, scaleX * rotY, 0, 0,
			scaleY * -rotY, scaleY * rotX, 0, 0,
			0, 0, 1, 0,
			sposx, sposy, 0, 1
		);
		_DefaultShotRender(pData, pFrame, matTransform, color);
	};

	if (anim_.type) {
		switch (anim_.type) {
		case (AnimParameter::SHOTANIM_JIGGLE):
			// args -> total time, speed, scale amount, _
			// time decrements, accumulate used
			{
				double sc[2];
				Math::DoSinCos(Math::DegreeToRadian(anim_.accumulate), sc);
				float decel = Math::Lerp::Decelerate<float, float>(0.0f, 1.0f, (anim_.args[0] - anim_.time) / anim_.args[0]);
				float bell = (1.0f - cos(GM_PI_X2 * decel)) / 2;
				float amplitude = anim_.args[2] * bell;
				scaleX *= 1.0f + amplitude * sc[1];
				scaleY *= 1.0f + amplitude * sc[0];
			}
			break;
		case (AnimParameter::SHOTANIM_SQUISH):
			// args -> speed, scale amount, _, _
			{
				double sc[2];
				Math::DoSinCos(Math::DegreeToRadian(anim_.args[0] * (anim_.time + frameExist_)), sc);
				scaleX *= 1.0f + anim_.args[1] * sc[0];
				scaleY *= 1.0f + anim_.args[1] * sc[1];
			}
			break;
		case (AnimParameter::SHOTANIM_FLUTTER):
			// args -> speed, move amount, scale offset, scale amount
			// accumulate used
			{
				float amplitude = sin(Math::DegreeToRadian(anim_.accumulate + anim_.time)) - anim_.args[2];
				scaleX *= 1.0f + amplitude * anim_.args[3];
				scaleY *= 1.0f + amplitude * anim_.args[3] * 0.1f; // this 0.1f is arbitrary
			}
			break;
		case (AnimParameter::SHOTANIM_DANCE):
			// args -> speed, angle amount, scale amount, scale oscillation rate
			{
				float timeOff = Math::DegreeToRadian(anim_.args[0] * (anim_.time + frameExist_));
				double sc[2];
				Math::DoSinCos(Math::DegreeToRadian(anim_.args[1]) * sin(timeOff), sc);
				rotX = move_.x * sc[1] - move_.y * sc[0];
				rotY = move_.x * sc[0] + move_.y * sc[1];
				float dance = anim_.args[2] * sinf(timeOff * anim_.args[3]);
				scaleX *= 1.0f + dance;
				scaleY *= 1.0f - dance;
			}
			break;
		}
	}

	if (delay_.time > 0) {
		BlendMode objBlendType = GetDelayBlendType();
		objBlendType = objBlendType == MODE_BLEND_NONE ? shotData->GetDelayRenderType() : objBlendType;
		if (objBlendType != targetBlend) return;

		StgShotData* delayData = _GetShotData(delay_.id >= 0 ? delay_.id : shotData->GetDefaultDelayID());
		if (delayData) {
			StgShotDataFrame* delayFrame = delayData ? delayData->GetFrame(frameWork_) : nullptr;

			scaleX *= delay_.GetScale();
			scaleY *= delay_.GetScale();
			if (delay_.scaleMix) {
				scaleX *= scale_.x;
				scaleY *= scale_.y;
			}

			color = (delay_.colorRep != 0) ? delay_.colorRep : shotData->GetDelayColor();
			if (delay_.colorMix) ColorAccess::MultiplyColor(color, color_);
			{
				float alphaRate = delay_.GetAlpha();
				if (frameFadeDelete_ >= 0) {
					float lerp = std::clamp<float>(1.0f - (float)frameFadeDelete_ / FRAME_FADEDELETE, 0, 1);
					alphaRate = std::clamp<float>(Math::Lerp::Accelerate<float, float>(alphaRate, alphaRate - 1.0f, lerp), 0, 1);
					scaleX = Math::Lerp::Smooth<float, float>(scaleX, 0, lerp);
					scaleY = Math::Lerp::Smooth<float, float>(scaleY, 0, lerp);
				}
				byte alpha = ColorAccess::ClampColorRet(((color >> 24) & 0xff) * alphaRate);
				color = (color & 0x00ffffff) | (alpha << 24);
			}

			_Render(delayData, delayFrame);
		}
	}
	else {
		BlendMode objBlendType = GetBlendType();
		objBlendType = objBlendType == MODE_BLEND_NONE ? shotData->GetRenderType() : objBlendType;
		if (objBlendType != targetBlend) return;

		scaleX *= scale_.x;
		scaleY *= scale_.y;
		color = color_;

		{
			float alphaRate = shotData->GetAlpha() / 255.0f;
			if (frameFadeDelete_ >= 0) {
				float lerp = std::clamp<float>(1.0f - (float)frameFadeDelete_ / FRAME_FADEDELETE, 0, 1);
				alphaRate = std::clamp<float>(Math::Lerp::Accelerate<float, float>(alphaRate, alphaRate - 1.0f, lerp), 0, 1);
				scaleX = Math::Lerp::Smooth<float, float>(scaleX, 0, lerp);
				scaleY = Math::Lerp::Smooth<float, float>(scaleY, 0, lerp);
			}
			byte alpha = ColorAccess::ClampColorRet(((color >> 24) & 0xff) * alphaRate);
			color = (color & 0x00ffffff) | (alpha << 24);
		}

		StgShotDataFrame* shotFrame = shotData->GetFrame(frameWork_);
		_Render(shotData, shotFrame);
	}

	//if (bIntersected_) color = D3DCOLOR_ARGB(255, 255, 0, 0);
}

void StgNormalShotObject::_SendDeleteEvent(TypeDelete type) {
	if (typeOwner_ != OWNER_ENEMY) return;

	auto stageScriptManager = stageController_->GetScriptManager();
	auto objectManager = stageController_->GetMainObjectManager();

	StgShotManager* shotManager = stageController_->GetShotManager();
	StgItemManager* itemManager = stageController_->GetItemManager();

	if (!shotManager->IsDeleteEventEnable(type)) return;

	{
		int typeEvent = StgShotManager::_TypeDeleteToEventType(type);

		{
			Math::DVec2 pos{ GetPositionX(), GetPositionY() };

			LOCK_WEAK(itemScript, stageScriptManager->GetItemScript()) {
				gstd::value listScriptValue[3];
				listScriptValue[0] = DxScript::CreateIntValue(idObject_);
				listScriptValue[1] = DxScript::CreateFloatArrayValue(pos);
				listScriptValue[2] = DxScript::CreateIntValue(GetShotDataID());
				itemScript->RequestEvent(typeEvent, listScriptValue, 3);
			}

			//Create default delete item
			if (type == TypeDelete::Item && itemManager->IsDefaultBonusItemEnable()) {
				if (itemManager->GetItemCount() < StgItemManager::ITEM_MAX) {
					ref_unsync_ptr<StgItemObject> obj(new StgItemObject_Bonus(stageController_));

					int id = objectManager->AddObject(obj);
					if (id != DxScript::ID_INVALID) {
						itemManager->AddItem(obj);
						obj->SetPositionX(pos[0]);
						obj->SetPositionY(pos[1]);
					}
				}
			}
		}
	}
}

void StgNormalShotObject::SetShotDataID(int id) {
	bool bPreserveSpin = id < 0;	//If id is negative, preserve angle and spin

	StgShotData* oldData = _GetShotData();
	StgShotObject::SetShotDataID(abs(id));

	StgShotData* shotData = _GetShotData();
	if (shotData != nullptr && oldData != shotData) {
		double newSpin = 0;

		double avMin = shotData->GetAngularVelocityMin();
		double avMax = shotData->GetAngularVelocityMax();
		if (avMin != avMax) {
			ref_count_ptr<StgStageInformation> stageInfo = stageController_->GetStageInformation();
			shared_ptr<RandProvider> rand = stageInfo->GetRandProvider();
			newSpin = rand->GetReal(avMin, avMax);
		}
		else newSpin = avMin;

		if (!bPreserveSpin) {
			angularVelocity_ = newSpin;
			angle_.z = 0;
		}
		bFixedAngle_ = shotData->IsFixedAngle();
	}
}

//****************************************************************************
//StgLaserObject(レーザー基本部)
//****************************************************************************
StgLaserObject::StgLaserObject(StgStageController* stageController) : StgShotObject(stageController) {
	life_ = 9999999;
	bSpellResist_ = true;

	length_ = 0;
	lengthF_ = 0;
	widthRender_ = 0;
	widthIntersection_ = -1;
	extendRate_ = 0;
	maxLength_ = 0;
	invalidLengthStart_ = 0.1f;
	invalidLengthEnd_ = 0.1f;
	frameGrazeInvalidStart_ = 20;
	itemDistance_ = 24;

	delay_ = DelayParameter(0.5, 3.5, 15.0f);

	move_ = D3DXVECTOR2(1, 0);
	lastAngle_ = 0;
}

void StgLaserObject::Clone(DxScriptObjectBase* _src, bool deepCopy) {
	StgShotObject::Clone(_src, deepCopy);

	auto src = (StgLaserObject*)_src;

	length_ = src->length_;
	widthRender_ = src->widthRender_;
	widthIntersection_ = src->widthIntersection_;

	invalidLengthStart_ = src->invalidLengthStart_;
	invalidLengthEnd_ = src->invalidLengthEnd_;
	itemDistance_ = src->itemDistance_;
}

void StgLaserObject::_AddIntersectionRelativeTarget() {
	StgIntersectionManager* intersectionManager = stageController_->GetIntersectionManager();

	if ((IsDeleted() || (delay_.time > 0 && !bEnableMotionDelay_) || frameFadeDelete_ >= 0)
		|| (bUserIntersectionMode_ || !bIntersectionEnable_)
		|| pOwnReference_.expired() || widthIntersection_ <= 0)
		return;

	StgShotData* shotData = _GetShotData();
	if (shotData == nullptr)
		return;

	ClearIntersected();

	bool res = GetIntersectionTargetList_NoVector(shotData);
	if (res) {
		for (auto& iTarget : listIntersectionTarget_) {
			if (iTarget.first && iTarget.second != nullptr)
				intersectionManager->AddTarget(iTarget.second);
		}
	}
}
StgIntersectionObject::IntersectionListType StgLaserObject::GetIntersectionTargetList() {
	if ((IsDeleted() || (delay_.time > 0 && !bEnableMotionDelay_) || frameFadeDelete_ >= 0)
		|| (bUserIntersectionMode_ || !bIntersectionEnable_)
		|| pOwnReference_.expired() || widthIntersection_ <= 0)
		return IntersectionListType();

	StgShotData* shotData = _GetShotData();
	if (shotData == nullptr)
		return IntersectionListType();

	bool res = GetIntersectionTargetList_NoVector(shotData);
	if (res) return listIntersectionTarget_;

	return IntersectionListType();
}

void StgLaserObject::_ExtendLength() {
	if (extendRate_ != 0) {
		lengthF_ += extendRate_;

		if (extendRate_ > 0)
			lengthF_ = std::min(lengthF_, (float)maxLength_);
		else
			lengthF_ = std::max(lengthF_, (float)maxLength_);

		length_ = (int)lengthF_;
	}
}

//****************************************************************************
//StgLooseLaserObject(射出型レーザー)
//****************************************************************************
StgLooseLaserObject::StgLooseLaserObject(StgStageController* stageController) : StgLaserObject(stageController) {
	typeObject_ = TypeObject::LooseLaser;

	posTail_ = { 0, 0 };
	posOrigin_ = D3DXVECTOR2(0, 0);

	currentLength_ = 0;

	listIntersectionTarget_.push_back(CreateEmptyIntersection());
}

void StgLooseLaserObject::Clone(DxScriptObjectBase* _src, bool deepCopy) {
	StgLaserObject::Clone(_src, deepCopy);

	auto src = (StgLooseLaserObject*)_src;

	posTail_ = src->posTail_;
	posOrigin_ = src->posOrigin_;

	currentLength_ = src->currentLength_;
}

void StgLooseLaserObject::Work() {
	if (frameWork_ == 0) {
		posTail_[0] = posOrigin_.x = posX_;
		posTail_[1] = posOrigin_.y = posY_;
	}

	if (bEnableMovement_) {
		_Move();


		if (delay_.time > 0) {
			--(delay_.time);
			delay_.angle.x += delay_.angle.y;
		}
	}

	_CommonWorkTask();
	//	_AddIntersectionRelativeTarget();
}
void StgLooseLaserObject::_Move() {
	if (delay_.time == 0 || bEnableMotionDelay_) {
		StgMoveObject::_Move();
		_ExtendLength();
	}
	DxScriptRenderObject::SetX(posX_);
	DxScriptRenderObject::SetY(posY_);

	double angleZ = GetDirectionAngle();

	if (delay_.time <= 0 || bEnableMotionDelay_) {
		currentLength_ = hypot(posTail_[0] - posX_, posTail_[1] - posY_);
		if (currentLength_ >= length_) {
			//float speed = GetSpeed();
			posTail_[0] = posX_ - length_ * move_.x;
			posTail_[1] = posY_ - length_ * move_.y;
		}
	}
	if (lastAngle_ != angleZ) {
		lastAngle_ = angleZ;
		move_ = D3DXVECTOR2(cosf(lastAngle_), sinf(lastAngle_));
	}
}
void StgLooseLaserObject::_DeleteInAutoClip() {
	if (IsDeleted() || !IsAutoDelete()) return;

	DxRect<LONG>* const rcStgFrame = stageController_->GetStageInformation()->GetStgFrameRect();
	DxRect<LONG>* const rcClipBase = stageController_->GetShotManager()->GetShotDeleteClip();
	DxRect<LONG> rcDeleteClip(rcClipBase->left, rcClipBase->top,
		rcStgFrame->GetWidth() + rcClipBase->right,
		rcStgFrame->GetHeight() + rcClipBase->bottom);

	if (!rcDeleteClip.IsPointIntersected(posX_, posY_) && !rcDeleteClip.IsPointIntersected(posTail_)) {
		auto objectManager = stageController_->GetMainObjectManager();
		objectManager->DeleteObject(this);
	}
}

bool StgLooseLaserObject::GetIntersectionTargetList_NoVector(StgShotData* shotData) {
	if (abs(hitboxScale_.x) < 0.01f)
		return false;

	float invLengthS = (1.0f - (1.0f - invalidLengthStart_) * hitboxScale_.y) * 0.5f;
	float invLengthE = (1.0f - (1.0f - invalidLengthEnd_) * hitboxScale_.y) * 0.5f;

	float lineXS = Math::Lerp::Linear(posX_, posTail_[0], invLengthS);
	float lineYS = Math::Lerp::Linear(posY_, posTail_[1], invLengthS);
	float lineXE = Math::Lerp::Linear(posTail_[0], posX_, invLengthE);
	float lineYE = Math::Lerp::Linear(posTail_[1], posY_, invLengthE);

	{
		IntersectionPairType* pPair = &listIntersectionTarget_[0];

		StgIntersectionTarget_Line* pTarget = (StgIntersectionTarget_Line*)(pPair->second.get());
		if (pTarget == nullptr) {
			pTarget = new StgIntersectionTarget_Line();
			pPair->second.reset(pTarget);
		}
		pPair->first = true;

		DxWidthLine* pDstLine = &pTarget->GetLine();
		*pDstLine = DxWidthLine(lineXS, lineYS, lineXE, lineYE, widthIntersection_ * hitboxScale_.x);

		pTarget->SetTargetType(typeOwner_ == OWNER_PLAYER ?
			StgIntersectionTarget::TYPE_PLAYER_SHOT : StgIntersectionTarget::TYPE_ENEMY_SHOT);
		pTarget->SetObject(pOwnReference_);
		pTarget->SetIntersectionSpace();
	}

	return true;
}

void StgLooseLaserObject::Render(BlendMode targetBlend) {
	//if (!IsVisible()) return;
	StgShotManager* shotManager = stageController_->GetShotManager();

	StgShotData* shotData = _GetShotData();
	if (shotData == nullptr) return;

	D3DXVECTOR2 rPos;
	D3DXVECTOR2 rScale;
	D3DXVECTOR2 rAngle;		//[cos, sin]
	D3DCOLOR rColor;

	//Render delay
	if (delay_.time > 0) {
		BlendMode objBlendType = GetDelayBlendType();
		objBlendType = objBlendType == MODE_BLEND_NONE ? MODE_BLEND_ADD_ARGB : objBlendType;

		if (objBlendType == targetBlend) {
			StgShotData* delayData = _GetShotData(delay_.id >= 0 ? delay_.id : shotData->GetDefaultDelayID());
			if (delayData) {
				StgShotDataFrame* delayFrame = delayData ? delayData->GetFrame(frameWork_) : nullptr;

				rPos = bEnableMotionDelay_ ? posOrigin_ : D3DXVECTOR2(position_);
				if (bRoundingPosition_) {
					rPos.x = roundf(rPos.x);
					rPos.y = roundf(rPos.y);
				}
				rScale.x = rScale.y = delay_.GetScale();
				rAngle = (delay_.angle.y != 0) ? D3DXVECTOR2(cosf(delay_.angle.x), sinf(delay_.angle.x)) : move_;

				rColor = (delay_.colorRep != 0) ? delay_.colorRep : shotData->GetDelayColor();
				if (delay_.colorMix) ColorAccess::MultiplyColor(rColor, color_);
				{
					byte alpha = ColorAccess::ClampColorRet(((rColor >> 24) & 0xff) * delay_.GetAlpha());
					rColor = (rColor & 0x00ffffff) | (alpha << 24);
				}

				D3DXMATRIX matTransform(
					rScale.x * rAngle.x, rScale.x * rAngle.y, 0, 0,
					rScale.y * -rAngle.y, rScale.y * rAngle.x, 0, 0,
					0, 0, 1, 0,
					rPos.x, rPos.y, 0, 1
				);
				_DefaultShotRender(delayData, delayFrame, matTransform, rColor);
			}
		}
	}

	//Render laser
	if (delay_.time == 0 || bEnableMotionDelay_) {
		BlendMode objBlendType = GetBlendType();
		objBlendType = objBlendType == MODE_BLEND_NONE ? MODE_BLEND_ADD_ARGB : objBlendType;

		if (objBlendType == targetBlend) {
			StgShotDataFrame* shotFrame = shotData->GetFrame(frameWork_);

			float dx = posTail_[0] - posX_;
			float dy = posTail_[1] - posY_;

			if (currentLength_ > 0 && widthRender_ != 0) {
				rPos = D3DXVECTOR2(posX_ + posTail_[0], posY_ + posTail_[1]) / 2;	//Render from the laser center
				if (bRoundingPosition_) {
					rPos.x = roundf(rPos.x);
					rPos.y = roundf(rPos.y);
				}

				DxRect<float>* rcDst = shotFrame->GetDestRect();
				rScale.x = widthRender_ / rcDst->GetWidth() * scale_.x;
				rScale.y = currentLength_ / rcDst->GetHeight() * scale_.y;

				rAngle = D3DXVECTOR2(dy, -dx) / currentLength_;

				rColor = color_;
				{
					float alphaRate = shotData->GetAlpha() / 255.0f;
					if (frameFadeDelete_ >= 0) {
						float lerp = std::clamp<float>(1.0f - (float)frameFadeDelete_ / FRAME_FADEDELETE, 0, 1);
						alphaRate = std::clamp<float>(Math::Lerp::Accelerate<float, float>(alphaRate, alphaRate - 1.0f, lerp), 0, 1);
						rScale.x = Math::Lerp::Smooth<float, float>(rScale.x, 0, lerp);
					}
					byte alpha = ColorAccess::ClampColorRet(((rColor >> 24) & 0xff) * alphaRate);
					rColor = (rColor & 0x00ffffff) | (alpha << 24);
				}

				D3DXMATRIX matTransform(
					rScale.x * rAngle.x, rScale.x * rAngle.y, 0, 0,
					rScale.y * -rAngle.y, rScale.y * rAngle.x, 0, 0,
					0, 0, 1, 0,
					rPos.x, rPos.y, 0, 1
				);
				_DefaultShotRender(shotData, shotFrame, matTransform, rColor);
			}

		}
	}
}

void StgLooseLaserObject::_SendDeleteEvent(TypeDelete type) {
	if (typeOwner_ != OWNER_ENEMY) return;

	auto stageScriptManager = stageController_->GetScriptManager();
	auto objectManager = stageController_->GetMainObjectManager();

	StgShotManager* shotManager = stageController_->GetShotManager();
	StgItemManager* itemManager = stageController_->GetItemManager();

	if (!shotManager->IsDeleteEventEnable(type)) return;

	auto itemScript = stageScriptManager->GetItemScript().lock();

	{
		int typeEvent = StgShotManager::_TypeDeleteToEventType(type);

		double ex = GetPositionX();
		double ey = GetPositionY();

		Math::DVec2 pos;
		gstd::value listScriptValue[3];

		for (double itemPos = 0; itemPos < currentLength_; itemPos += itemDistance_) {
			pos = { ex - itemPos * move_.x, ey - itemPos * move_.y };

			if (itemScript) {
				gstd::value listScriptValue[3];
				listScriptValue[0] = DxScript::CreateIntValue(idObject_);
				listScriptValue[1] = DxScript::CreateFloatArrayValue(pos);
				listScriptValue[2] = DxScript::CreateIntValue(GetShotDataID());
				itemScript->RequestEvent(typeEvent, listScriptValue, 3);
			}

			//Create default delete item
			if (type == TypeDelete::Item && itemManager->IsDefaultBonusItemEnable()) {
				if (delay_.time == 0 || bEnableMotionDelay_) {
					if (itemManager->GetItemCount() < StgItemManager::ITEM_MAX) {
						ref_unsync_ptr<StgItemObject> obj(new StgItemObject_Bonus(stageController_));

						int id = objectManager->AddObject(obj);
						if (id != DxScript::ID_INVALID) {
							itemManager->AddItem(obj);
							obj->SetPositionX(pos[0]);
							obj->SetPositionY(pos[1]);
						}
					}
				}
			}
		}
	}
}

//****************************************************************************
//StgStraightLaserObject(設置型レーザー)
//****************************************************************************
StgStraightLaserObject::StgStraightLaserObject(StgStageController* stageController) : StgLaserObject(stageController) {
	typeObject_ = TypeObject::StraightLaser;

	angLaser_ = 0;
	angVelLaser_ = 0;
	frameFadeDelete_ = -1;

	bUseSouce_ = true;
	bUseEnd_ = false;
	idImageEnd_ = -1;

	delaySize_ = D3DXVECTOR2(1, 1);

	scaleX_ = 0.05f;

	bLaserExpand_ = true;

	springMassIndexes2_.clear();

	move_ = D3DXVECTOR2(1, 0);

	listIntersectionTarget_.push_back(CreateEmptyIntersection());
}

void StgStraightLaserObject::Clone(DxScriptObjectBase* _src, bool deepCopy) {
	StgLaserObject::Clone(_src, deepCopy);

	auto src = (StgStraightLaserObject*)_src;

	angLaser_ = src->angLaser_;

	bUseSouce_ = src->bUseSouce_;
	bUseEnd_ = src->bUseEnd_;
	idImageEnd_ = src->idImageEnd_;

	delaySize_ = src->delaySize_;
	scaleX_ = src->scaleX_;
	bLaserExpand_ = src->bLaserExpand_;
}

void StgStraightLaserObject::Work() {
	if (frameWork_ == 0 && delay_.time == 0) 
		scaleX_ = 1.0f;

	if (bEnableMovement_) {
		if (!springMassSystems_.empty()) {
			DxSpringMassSystemObjectParticle* particle1 = springMassSystems_[0]->GetParticle(springMassIndexes_[0]);
			DxSpringMassSystemObjectParticle* particle2 = springMassSystems_[0]->GetParticle(springMassIndexes2_[0]);
			posX_ = particle1->pos[0];
			posY_ = particle1->pos[1];
			double dx = particle2->pos[0] - posX_;
			double dy = particle2->pos[1] - posY_;
			angLaser_ = atan2(dy, dx);
			SetLength(hypot(dy, dx));
			UpdateRelativePosition();
			SetX(posX_);
			SetY(posY_);
		}
		else {
			_Move();
			_ExtendLength();

			if (angVelLaser_ != 0) angLaser_ += angVelLaser_;

			if (!bLaserExpand_ || delay_.time > 0) {
				if (delay_.time > 0) --(delay_.time);
				scaleX_ = std::max(0.05f, scaleX_ - 0.1f);
			}
			else if (bLaserExpand_)
				scaleX_ = std::min(1.0f, scaleX_ + 0.1f);
		}

		delay_.angle.x += delay_.angle.y;

		if (lastAngle_ != angLaser_) {
			lastAngle_ = angLaser_;
			move_ = D3DXVECTOR2(cosf(angLaser_), sinf(angLaser_));
		}
	}

	_CommonWorkTask();
}

void StgStraightLaserObject::_DeleteInAutoClip() {
	if (IsDeleted() || !IsAutoDelete()) return;

	DxRect<LONG>* const rcStgFrame = stageController_->GetStageInformation()->GetStgFrameRect();
	DxRect<LONG>* const rcClipBase = stageController_->GetShotManager()->GetShotDeleteClip();
	DxRect<LONG> rcDeleteClip(rcClipBase->left, rcClipBase->top,
		rcStgFrame->GetWidth() + rcClipBase->right,
		rcStgFrame->GetHeight() + rcClipBase->bottom);

	double posXE = posX_ + length_ * move_.x;
	double posYE = posY_ + length_ * move_.y;

	if (!rcDeleteClip.IsPointIntersected(posX_, posY_) && !rcDeleteClip.IsPointIntersected(posXE, posYE)) {
		auto objectManager = stageController_->GetMainObjectManager();
		objectManager->DeleteObject(this);
	}
}
bool StgStraightLaserObject::GetIntersectionTargetList_NoVector(StgShotData* shotData) {
	if (scaleX_ < 1 && typeOwner_ != OWNER_PLAYER)
		return false;

	float length = length_ * hitboxScale_.y;
	if (abs(hitboxScale_.x) < 0.01f || abs(length) < 0.01f)
		return false;

	double posXE = posX_ + length * move_.x;
	double posYE = posY_ + length * move_.y;
	float invLenHalfS = invalidLengthStart_ * 0.5f;
	float invLenHalfE = invalidLengthEnd_ * 0.5f;

	float lineXS = Math::Lerp::Linear(posX_, posXE, invLenHalfS);
	float lineYS = Math::Lerp::Linear(posY_, posYE, invLenHalfS);
	float lineXE = Math::Lerp::Linear(posXE, posX_, invLenHalfE);
	float lineYE = Math::Lerp::Linear(posYE, posY_, invLenHalfE);

	{
		IntersectionPairType* pPair = &listIntersectionTarget_[0];

		StgIntersectionTarget_Line* pTarget = (StgIntersectionTarget_Line*)(pPair->second.get());
		if (pTarget == nullptr) {
			pTarget = new StgIntersectionTarget_Line();
			pPair->second.reset(pTarget);
		}
		pPair->first = true;

		DxWidthLine* pDstLine = &pTarget->GetLine();
		*pDstLine = DxWidthLine(lineXS, lineYS, lineXE, lineYE, widthIntersection_ * hitboxScale_.x);

		pTarget->SetTargetType(typeOwner_ == OWNER_PLAYER ?
			StgIntersectionTarget::TYPE_PLAYER_SHOT : StgIntersectionTarget::TYPE_ENEMY_SHOT);
		pTarget->SetObject(pOwnReference_);
		pTarget->SetIntersectionSpace();
	}

	return true;
}

void StgStraightLaserObject::Render(BlendMode targetBlend) {
	//if (!IsVisible()) return;
	StgShotManager* shotManager = stageController_->GetShotManager();

	StgShotData* shotData = _GetShotData();
	if (shotData == nullptr) return;

	D3DCOLOR rColor;

	//Render laser
	{
		BlendMode objBlendType = GetBlendType();
		objBlendType = objBlendType == MODE_BLEND_NONE ? MODE_BLEND_ADD_ARGB : objBlendType;

		if (objBlendType == targetBlend) {
			StgShotDataFrame* shotFrame = shotData->GetFrame(frameWork_);

			D3DXVECTOR2 rAngle(move_.y, -move_.x);

			float _renderWd = std::max<float>(abs(widthRender_) * scaleX_, 2.0f) * scale_.x;
			float _renderLn = length_ * scale_.y;

			//Render from the laser center
			D3DXVECTOR2 rPos = D3DXVECTOR2(posX_ * 2 + move_.x * _renderLn, posY_ * 2 + move_.y * _renderLn) / 2;
			if (bRoundingPosition_) {
				rPos.x = roundf(rPos.x);
				rPos.y = roundf(rPos.y);
			}

			DxRect<float>* rcDst = shotFrame->GetDestRect();
			D3DXVECTOR2 rScale(_renderWd / rcDst->GetWidth(), _renderLn / rcDst->GetHeight());

			rColor = color_;
			{
				float alphaRate = shotData->GetAlpha() / 255.0f;
				if (frameFadeDelete_ >= 0) {
					float lerp = std::clamp<float>(1.0f - (float)frameFadeDelete_ / FRAME_FADEDELETE_LASER, 0, 1);
					alphaRate = std::clamp<float>(Math::Lerp::Accelerate<float, float>(alphaRate, alphaRate - 1.0f, lerp), 0, 1);
					rScale.x = Math::Lerp::Smooth<float, float>(rScale.x, 0, lerp);
				}
				byte alpha = ColorAccess::ClampColorRet(((rColor >> 24) & 0xff) * alphaRate);
				rColor = (rColor & 0x00ffffff) | (alpha << 24);
			}

			D3DXMATRIX matTransform(
				rScale.x * -rAngle.x, rScale.x * -rAngle.y, 0, 0,
				rScale.y * rAngle.y, rScale.y * -rAngle.x, 0, 0,
				0, 0, 1, 0,
				rPos.x, rPos.y, 0, 1
			);
			_DefaultShotRender(shotData, shotFrame, matTransform, rColor);
		}
	}

	//Render delay(s)
	if ((bUseSouce_ || bUseEnd_) && (frameFadeDelete_ < 0)) {
		BlendMode objBlendType = GetDelayBlendType();
		objBlendType = objBlendType == MODE_BLEND_NONE ? MODE_BLEND_ADD_ARGB : objBlendType;

		if (objBlendType == targetBlend) {
			rColor = (delay_.colorRep != 0) ? delay_.colorRep : shotData->GetDelayColor();
			if (delay_.colorMix) ColorAccess::MultiplyColor(rColor, color_);

			const float delaySizeBase = widthRender_ * 4 / 3.0f;

			auto _AddDelay = [&](D3DXVECTOR2 delayPos, int delayID, float delaySize) {
				if (bRoundingPosition_) {
					delayPos.x = roundf(delayPos.x);
					delayPos.y = roundf(delayPos.y);
				}
				delaySize *= delaySizeBase;

				StgShotData* delayData = _GetShotData(delay_.id >= 0 ? delay_.id : shotData->GetDefaultDelayID());
				if (delayData) {
					StgShotDataFrame* delayFrame = delayData->GetFrame(frameWork_);

					D3DXVECTOR2 rAngle = (delay_.angle.y != 0) ? D3DXVECTOR2(cosf(delay_.angle.x), sinf(delay_.angle.x)) : move_;
					float rScaleX = delaySize / delayFrame->GetDestRect()->GetWidth();
					float rScaleY = delaySize / delayFrame->GetDestRect()->GetHeight();

					D3DXMATRIX matTransform(
						rScaleX * rAngle.x, rScaleX * rAngle.y, 0, 0,
						rScaleY * -rAngle.y, rScaleY * rAngle.x, 0, 0,
						0, 0, 1, 0,
						delayPos.x, delayPos.y, 0, 1
					);
					_DefaultShotRender(delayData, delayFrame, matTransform, rColor);
				}
			};

			if (bUseSouce_) {
				D3DXVECTOR2 delayPos(position_);
				_AddDelay(delayPos, delay_.id, delaySize_.x);
			}
			if (bUseEnd_) {
				D3DXVECTOR2 delayPos(position_.x + length_ * cosf(angLaser_),
					position_.y + length_ * sinf(angLaser_));
				_AddDelay(delayPos, idImageEnd_, delaySize_.y);
			}
		}
	}
}

void StgStraightLaserObject::_SendDeleteEvent(TypeDelete type) {
	if (typeOwner_ != OWNER_ENEMY) return;

	auto stageScriptManager = stageController_->GetScriptManager();
	auto objectManager = stageController_->GetMainObjectManager();

	StgShotManager* shotManager = stageController_->GetShotManager();
	StgItemManager* itemManager = stageController_->GetItemManager();

	if (!shotManager->IsDeleteEventEnable(type)) return;

	auto itemScript = stageScriptManager->GetItemScript().lock();

	{
		int typeEvent = StgShotManager::_TypeDeleteToEventType(type);

		Math::DVec2 pos;
		gstd::value listScriptValue[3];

		for (double itemPos = 0; itemPos < length_; itemPos += itemDistance_) {
			pos = { posX_ + itemPos * move_.x, posY_ + itemPos * move_.y };

			if (itemScript) {
				gstd::value listScriptValue[3];
				listScriptValue[0] = DxScript::CreateIntValue(idObject_);
				listScriptValue[1] = DxScript::CreateFloatArrayValue(pos);
				listScriptValue[2] = DxScript::CreateIntValue(GetShotDataID());
				itemScript->RequestEvent(typeEvent, listScriptValue, 3);
			}

			//Create default delete item
			if (type == TypeDelete::Item && itemManager->IsDefaultBonusItemEnable()) {
				if (delay_.time == 0) {
					if (itemManager->GetItemCount() < StgItemManager::ITEM_MAX) {
						ref_unsync_ptr<StgItemObject> obj(new StgItemObject_Bonus(stageController_));

						int id = objectManager->AddObject(obj);
						if (id != DxScript::ID_INVALID) {
							itemManager->AddItem(obj);
							obj->SetPositionX(pos[0]);
							obj->SetPositionY(pos[1]);
						}
					}
				}
			}
		}
	}
}

//****************************************************************************
//StgCurveLaserObject(曲がる型レーザー)
//****************************************************************************
StgCurveLaserObject::StgCurveLaserObject(StgStageController* stageController) : StgLaserObject(stageController) {
	typeObject_ = TypeObject::CurveLaser;
	tipDecrement_ = 0.0f;

	invalidLengthStart_ = 0.02f;
	invalidLengthEnd_ = 0.02f;

	itemDistance_ = 6.0f;

	bCap_ = false;
	bConnect_ = false;
	bUniformMove_ = false;
	smooth_ = 0;

	posOrigin_ = D3DXVECTOR2(0, 0);
}

void StgCurveLaserObject::Clone(DxScriptObjectBase* _src, bool deepCopy) {
	StgLaserObject::Clone(_src, deepCopy);

	auto src = (StgCurveLaserObject*)_src;

	listPosition_ = src->listPosition_;
	vertexData_ = src->vertexData_;
	listRectIncrement_ = src->listRectIncrement_;

	posXO_ = src->posXO_;
	posYO_ = src->posYO_;
	tipDecrement_ = src->tipDecrement_;
	bCap_ = src->bCap_;
	posOrigin_ = src->posOrigin_;
}

void StgCurveLaserObject::Work() {
	if (frameWork_ == 0) {
		posOrigin_.x = posX_;
		posOrigin_.y = posY_;
	}

	if (bEnableMovement_) {
		_Move();

		if (delay_.time > 0) {
			--(delay_.time);
			delay_.angle.x += delay_.angle.y;
		}
	}

	_CommonWorkTask();
	if (bConnect_) _UpdateConnectedPositionList();
	//	_AddIntersectionRelativeTarget();
}
void StgCurveLaserObject::_Move() {
	if (delay_.time == 0 || bEnableMotionDelay_) {
		StgMoveObject::_Move();
		_ExtendLength();
	}
	DxScriptRenderObject::SetX(posX_);
	DxScriptRenderObject::SetY(posY_);

	
	if (bUniformMove_) {
		D3DXVECTOR2 vec = D3DXVECTOR2(GetSpeedX(), GetSpeedY());
		for (LaserNode& iNode : listPosition_) {
			iNode.pos += vec;
		}
	}
	else {
		double angleZ = GetDirectionAngle();
		if (lastAngle_ != angleZ) {
			lastAngle_ = angleZ;
			move_ = D3DXVECTOR2(cosf(lastAngle_), sinf(lastAngle_));
		}

		D3DXVECTOR2 newNodePos(posX_, posY_);
		D3DXVECTOR2 newNodeVertF(-move_.y, move_.x);	//90 degrees rotation
		PushNode(CreateNode(newNodePos, newNodeVertF, 1.0f));
	}
}

StgCurveLaserObject::LaserNode StgCurveLaserObject::CreateNode(const D3DXVECTOR2& pos, const D3DXVECTOR2& rFac, float widthMul, D3DCOLOR col) {
	LaserNode node;
	node.parent = this;
	node.pos = pos;
	{
		float nx = rFac.x;
		float ny = rFac.y;
		node.vertOff[0] = { nx, ny };
		node.vertOff[1] = { -nx, -ny };
	}
	node.widthMul = widthMul;
	node.color = col;
	return node;
}
bool StgCurveLaserObject::GetNode(size_t indexNode, std::list<LaserNode>::iterator& res) {
	//Search from whichever end is closer to the target node index
	size_t listSizeMax = listPosition_.size();
	if (indexNode >= listSizeMax) res = listPosition_.end();
	if (indexNode < listSizeMax / 2U)
		res = std::next(listPosition_.begin(), indexNode);
	else
		res = std::next(listPosition_.rbegin(), listSizeMax - indexNode).base();
	return res != listPosition_.end();
}
void StgCurveLaserObject::GetNodePointerList(std::vector<LaserNode*>* listRes) {
	listRes->resize(listPosition_.size(), nullptr);
	size_t i = 0;
	for (LaserNode& iNode : listPosition_) {
		(*listRes)[i++] = &iNode;
	}
}
std::list<StgCurveLaserObject::LaserNode>::iterator StgCurveLaserObject::PushNode(const LaserNode& node) {
	listPosition_.push_front(node);

	while (listPosition_.size() > length_)
		listPosition_.pop_back();

	return listPosition_.begin();
}

void StgCurveLaserObject::_UpdateConnectedPositionList() {
	if (!listPosition_.empty()) {
		listPositionC_ = listPosition_;
		listPositionC_.push_back(listPosition_.front());
	}
}

void StgCurveLaserObject::_DeleteInAutoClip() {
	if (IsDeleted() || !IsAutoDelete()) return;
	StgShotManager* shotManager = stageController_->GetShotManager();

	DxRect<LONG>* const rcStgFrame = stageController_->GetStageInformation()->GetStgFrameRect();
	DxRect<LONG>* const rcClipBase = stageController_->GetShotManager()->GetShotDeleteClip();
	DxRect<LONG> rcDeleteClip(rcClipBase->left, rcClipBase->top,
		rcStgFrame->GetWidth() +          rcClipBase->right,
		rcStgFrame->GetHeight() + rcClipBase->bottom);

	//Checks if the node is within the bounding rect
	auto PredicateNodeInRect = [&](LaserNode& node) {
		return rcDeleteClip.IsPointIntersected((float*)&node.pos);
	};
	std::list<LaserNode>::iterator itrFind = std::find_if(listPosition_.begin(), listPosition_.end(),
		PredicateNodeInRect);

	//Can't find any node within the bounding rect
	if (itrFind == listPosition_.end()) {
		auto objectManager = stageController_->GetMainObjectManager();
		objectManager->DeleteObject(this);
	}
}
bool StgCurveLaserObject::GetIntersectionTargetList_NoVector(StgShotData* shotData) {
	if (abs(hitboxScale_.x) < 0.01f || abs(hitboxScale_.y) < 0.01f)
		return false;

	StgIntersectionManager* intersectionManager = stageController_->GetIntersectionManager();

	auto& listPos = bConnect_ ? listPositionC_ : listPosition_;

	size_t countPos = listPos.size();
	size_t countIntersection = countPos > 0U ? countPos - 1U : 0U;

	if (countIntersection == 0)
		return false;

	if (listIntersectionTarget_.size() < countIntersection)
		listIntersectionTarget_.resize(countIntersection, CreateEmptyIntersection());
	for (auto& i : listIntersectionTarget_) i.first = false;

	float iLengthS = invalidLengthStart_ * 0.5f;
	float iLengthE = 0.5f + (1.0f - invalidLengthEnd_) * 0.5f;
	int posInvalidS = (int)(countPos * iLengthS);
	int posInvalidE = (int)(countPos * iLengthE);
	float iWidth = widthIntersection_ * hitboxScale_.x;

	std::list<LaserNode>::iterator itr = listPos.begin();
	for (size_t iPos = 0; iPos < countIntersection; ++iPos, ++itr) {
		IntersectionPairType* pPair = &listIntersectionTarget_[iPos];

		if ((int)iPos < posInvalidS || (int)iPos > posInvalidE) {
			pPair->first = false;
			continue;
		}

		StgIntersectionTarget_Line* pTarget = (StgIntersectionTarget_Line*)(pPair->second.get());
		if (pTarget == nullptr) {
			pTarget = new StgIntersectionTarget_Line();
			pPair->second.reset(pTarget);
		}
		pPair->first = true;

		std::list<LaserNode>::iterator itrNext = std::next(itr);
		D3DXVECTOR2* nodeS = &itr->pos;
		D3DXVECTOR2* nodeE = &itrNext->pos;

		DxWidthLine* pDstLine = &pTarget->GetLine();
		*pDstLine = DxWidthLine(nodeS->x, nodeS->y, nodeE->x, nodeE->y, iWidth);

		pTarget->SetTargetType(typeOwner_ == OWNER_PLAYER ?
			StgIntersectionTarget::TYPE_PLAYER_SHOT : StgIntersectionTarget::TYPE_ENEMY_SHOT);
		pTarget->SetObject(pOwnReference_);
		pTarget->SetIntersectionSpace();
	}

	return true;
}

void StgCurveLaserObject::Render(BlendMode targetBlend) {
	//if (!IsVisible()) return;
	StgShotManager* shotManager = stageController_->GetShotManager();

	StgShotData* shotData = _GetShotData();
	if (shotData == nullptr) return;

	//Render delay
	if (delay_.time > 0) {
		BlendMode objBlendType = GetDelayBlendType();
		objBlendType = objBlendType == MODE_BLEND_NONE ? MODE_BLEND_ADD_ARGB : objBlendType;

		if (objBlendType == targetBlend) {
			StgShotData* delayData = _GetShotData(delay_.id >= 0 ? delay_.id : shotData->GetDefaultDelayID());
			if (delayData) {
				StgShotDataFrame* shotFrame = delayData->GetFrame(frameWork_);

				D3DXVECTOR2 rScale;
				D3DXVECTOR2 rAngle;		//[cos, sin]
				D3DCOLOR rColor;

				D3DXVECTOR2 rPos = bEnableMotionDelay_ ? posOrigin_ : D3DXVECTOR2(position_);
				if (bRoundingPosition_) {
					rPos.x = roundf(rPos.x);
					rPos.y = roundf(rPos.y);
				}
				rScale.x = rScale.y = delay_.GetScale();
				rAngle = (delay_.angle.y != 0) ? D3DXVECTOR2(cosf(delay_.angle.x), sinf(delay_.angle.x)) : move_;

				rColor = (delay_.colorRep != 0) ? delay_.colorRep : shotData->GetDelayColor();
				if (delay_.colorMix) ColorAccess::MultiplyColor(rColor, color_);
				{
					byte alpha = ColorAccess::ClampColorRet(((rColor >> 24) & 0xff) * delay_.GetAlpha());
					rColor = (rColor & 0x00ffffff) | (alpha << 24);
				}

				D3DXMATRIX matTransform(
					rScale.x * rAngle.x, rScale.x * rAngle.y, 0, 0,
					rScale.y * -rAngle.y, rScale.y * rAngle.x, 0, 0,
					0, 0, 1, 0,
					rPos.x, rPos.y, 0, 1
				);
				_DefaultShotRender(delayData, shotFrame, matTransform, rColor);
			}
		}
	}

	auto& listPos = bConnect_ ? listPositionC_ : listPosition_;

	//Render laser
	if (listPos.size() > 1U) {
		BlendMode objBlendType = GetBlendType();
		objBlendType = objBlendType == MODE_BLEND_NONE ? MODE_BLEND_ADD_ARGB : objBlendType;

		if (objBlendType == targetBlend) {
			StgShotDataFrame* shotFrame = shotData->GetFrame(frameWork_);

			size_t countPos = listPos.size();
			size_t countRect = countPos - 1U;
			size_t halfPos = countRect / 2U;

			shared_ptr<Texture> texture = shotFrame->GetVertexBufferContainer()->GetTexture();
			D3DXVECTOR2 texSizeInv = D3DXVECTOR2(1.0f / texture->GetWidth(), 1.0f / std::max<UINT>(texture->GetHeight(), 1));

			const DxRect<LONG>* rcSrcOrg = shotFrame->GetSourceRect();
			const LONG* ptrSrc = reinterpret_cast<const LONG*>(rcSrcOrg);

			float alphaRateShot = shotData->GetAlpha() / 255.0f;
			float scaleRateShot = 1.0f;
			if (frameFadeDelete_ >= 0) {
				float lerp = std::clamp<float>(1.0f - (float)frameFadeDelete_ / FRAME_FADEDELETE, 0, 1);
				alphaRateShot = std::clamp<float>(Math::Lerp::Accelerate<float, float>(alphaRateShot, alphaRateShot - 1.0f, lerp), 0, 1);
				scaleRateShot = Math::Lerp::Smooth<float, float>(scaleRateShot, 0, lerp);
			}

			float baseAlpha = (color_ >> 24) & 0xff;
			float tipAlpha = baseAlpha * (1.0f - tipDecrement_);

			float rcLen = rcSrcOrg->bottom - rcSrcOrg->top;
			float rcLenH = rcLen * 0.5f;

			float rcInc = (rcLen / (float)countRect) * texSizeInv.y;
			float rectV = rcSrcOrg->top * texSizeInv.y;

			float incDistFactor = rcLen * texSizeInv.y / widthRender_;
			float rcMidPt = rcLenH * texSizeInv.y;

			listRectIncrement_.resize(countPos);
			std::fill(listRectIncrement_.begin(), listRectIncrement_.end(), 0);
			{
				bool bCappable = false;
				if (bCap_) {
					// :WHAT:

					size_t i = 0;
					size_t iPos = 0;
					float remLen = rcMidPt;

					auto tryCap = [&](auto itr) -> bool {
						if (i > halfPos) // Auto-fails if cap crosses the half-way point
							return false;

						auto itrNext = std::next(itr);
						D3DXVECTOR2* pos = &itr->pos;
						D3DXVECTOR2* posNext = &itrNext->pos;
						// D3DXVECTOR2* off = &itr->vertOff[0];
						// float wid = std::max(hypotf(off->x, off->y) * 2, 1.0f);
						float incDist = hypotf(posNext->x - pos->x, posNext->y - pos->y) * incDistFactor;

						if (listRectIncrement_[iPos] == 0) // Fails if element was already written to
							listRectIncrement_[iPos] = std::min(incDist, remLen);
						else
							return false;

						remLen -= incDist;
						return true;
					};

					auto itrHead = listPos.begin();
					auto itrTail = listPos.rbegin();
					auto itrHeadEnd = listPos.rend();
					auto itrTailEnd = listPos.end();

					bCappable = true;
					for (auto itr = itrHead; bCappable && remLen > 0 && itr != itrTailEnd; ++itr, ++i, ++iPos)
						bCappable = tryCap(itr);

					i = 0;
					iPos = countPos - 2; // Ends straight up do not work otherwise?
					remLen = rcMidPt;
					for (auto itr = itrTail; bCappable && remLen > 0 && itr != itrHeadEnd; ++itr, ++i, --iPos)
						bCappable = tryCap(itr);
				}
				if (!bCappable) // If capping fails (or is disabled), just use the regular increment
					std::fill(listRectIncrement_.begin(), listRectIncrement_.end(), rcInc);
			}

			vertexData_.resize(countPos * 2U);

			float inv_halfPos = 1.0f / halfPos, inv_halfPosDec = 1.0f / (halfPos - 1);
			float halfWidthRender = widthRender_ / 2.0f;

				size_t iPos = 0U;
				for (auto itr = listPos.begin(); itr != listPos.end(); ++itr, ++iPos) {
					D3DXVECTOR2 pos = itr->pos;
					D3DXVECTOR2 vertOff[2]{ itr->vertOff[0], itr->vertOff[1] };

					if (smooth_ > 0 && countPos > 1) {
						auto itrNext = listPos.begin();
						auto itrPrev = listPos.begin();
						if (bConnect_) {
							std::advance(itrNext, (iPos + smooth_) % (countPos - 1));
							std::advance(itrPrev, (iPos - smooth_ + countPos - 1) % (countPos - 1));
						}
						else {
							std::advance(itrNext, std::clamp((int)iPos + smooth_, 0, (int)countPos - 1));
							std::advance(itrPrev, std::clamp((int)iPos - smooth_, 0, (int)countPos - 1));
						}

						D3DXVECTOR2* posNext = &itrNext->pos;
						D3DXVECTOR2* posPrev = &itrPrev->pos;

						float arc = atan2f(posNext->y - posPrev->y, posNext->x - posPrev->x);

						D3DXVECTOR2 vecNew(-sinf(arc), cosf(arc));
						vertOff[0] = vecNew;
						vertOff[1] = -vecNew;
					}

					float nodeAlpha = baseAlpha;
					if (iPos > halfPos)
						nodeAlpha = Math::Lerp::Linear(baseAlpha, tipAlpha, (iPos - halfPos + 1) * inv_halfPos);
					else if (iPos < halfPos)
						nodeAlpha = Math::Lerp::Linear(tipAlpha, baseAlpha, iPos * inv_halfPosDec);
					nodeAlpha = std::max(0.0f, nodeAlpha);

				float renderWd = std::max(halfWidthRender * itr->widthMul, 1.0f) * scale_.x * scaleRateShot;

				D3DCOLOR thisColor = 0xffffffff;
				{
					byte alpha = ColorAccess::ClampColorRet(nodeAlpha * alphaRateShot);
					thisColor = (thisColor & 0x00ffffff) | (alpha << 24);
				}
				if (itr->color != 0xffffffff) ColorAccess::MultiplyColor(thisColor, itr->color);

				for (size_t iVert = 0U; iVert < 2U; ++iVert) {
					VERTEX_TLX* pv = &vertexData_[iPos * 2 + iVert];

					_SetVertexUV(pv, ptrSrc[(iVert & 1) << 1] * texSizeInv.x, rectV);
					_SetVertexPosition(pv, itr->pos.x + vertOff[iVert].x * renderWd,
						itr->pos.y + vertOff[iVert].y * renderWd, position_.z);
					_SetVertexColorARGB(pv, thisColor);
				}

				rectV += listRectIncrement_[iPos];
			}

			{
				DirectGraphics* graphics = DirectGraphics::GetBase();
				IDirect3DDevice9* device = graphics->GetDevice();

				VertexBufferManager* vbManager = VertexBufferManager::GetBase();
				FixedVertexBuffer* vertexBuffer = vbManager->GetVertexBufferTLX();

				if (graphics->IsAllowRenderTargetChange()) {
					if (auto pRT = renderTarget_.lock())
						graphics->SetRenderTarget(pRT);
					else graphics->SetRenderTarget(nullptr);
				}

				graphics->SetCurrentTexture(texture->GetD3DTexture());

				size_t countVert = vertexData_.size();
				size_t countPrim = RenderObjectPrimitive::GetPrimitiveCount(D3DPT_TRIANGLESTRIP, countVert);

				{
					BufferLockParameter lockParam = BufferLockParameter(D3DLOCK_DISCARD);

					lockParam.SetSource(vertexData_, countVert, sizeof(VERTEX_TLX));
					vertexBuffer->UpdateBuffer(&lockParam);
				}

				device->SetStreamSource(0, vertexBuffer->GetBuffer(), 0, sizeof(VERTEX_TLX));

				{
					ID3DXEffect* effect = shotManager->GetEffect();
					if (shader_) {
						effect = shader_->GetEffect();
						if (shader_->LoadTechnique()) {
							shader_->LoadParameter();
						}
					}

					if (effect) {
						D3DXHANDLE handle = nullptr;
						if (handle = effect->GetParameterBySemantic(nullptr, "WORLD")) {
							effect->SetMatrix(handle, &graphics->GetCamera()->GetIdentity());
						}
						if (shader_) {
							if (handle = effect->GetParameterBySemantic(nullptr, "VIEWPROJECTION")) {
								effect->SetMatrix(handle, shotManager->GetProjectionMatrix());
							}
						}
						if (handle = effect->GetParameterBySemantic(nullptr, "ICOLOR")) {
							//To normalized RGBA vector
							D3DXVECTOR4 vColor = ColorAccess::ToVec4Normalized(color_, ColorAccess::PERMUTE_RGBA);
							effect->SetVector(handle, &vColor);
						}

						UINT countPass = 1;
						effect->Begin(&countPass, D3DXFX_DONOTSAVESHADERSTATE);
						for (UINT iPass = 0; iPass < countPass; ++iPass) {
							effect->BeginPass(iPass);
							device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, countPrim);
							effect->EndPass();
						}
						effect->End();
					}
				}
			}
		}
	}
}

void StgCurveLaserObject::_SendDeleteEvent(TypeDelete type) {
	if (typeOwner_ != OWNER_ENEMY) return;

	auto stageScriptManager = stageController_->GetScriptManager();
	auto objectManager = stageController_->GetMainObjectManager();

	StgShotManager* shotManager = stageController_->GetShotManager();
	StgItemManager* itemManager = stageController_->GetItemManager();

	if (!shotManager->IsDeleteEventEnable(type)) return;

	auto itemScript = stageScriptManager->GetItemScript().lock();

	{
		int typeEvent = StgShotManager::_TypeDeleteToEventType(type);

		gstd::value listScriptValue[3];
		listScriptValue[0] = DxScript::CreateIntValue(idObject_);
		listScriptValue[2] = DxScript::CreateIntValue(GetShotDataID());

		size_t countToItem = 0U;
		auto _RequestItem = [&](double ix, double iy) {
			if (itemScript) {
				listScriptValue[1] = itemScript->CreateFloatArrayValue(Math::DVec2{ ix, iy });
				itemScript->RequestEvent(typeEvent, listScriptValue, 3);
			}

			//Create default delete item
			if (type == TypeDelete::Item && itemManager->IsDefaultBonusItemEnable()) {
				if (delay_.time == 0 || bEnableMotionDelay_) {
					if (itemManager->GetItemCount() < StgItemManager::ITEM_MAX) {
						ref_unsync_ptr<StgItemObject> obj(new StgItemObject_Bonus(stageController_));

						int id = objectManager->AddObject(obj);
						if (id != DxScript::ID_INVALID) {
							itemManager->AddItem(obj);
							obj->SetPositionX(ix);
							obj->SetPositionY(iy);
						}
					}
				}
			}
			++countToItem;
		};

	float lengthAcc = 0.0;
	auto& listPos = bConnect_ ? listPositionC_ : listPosition_;
	for (std::list<LaserNode>::iterator itr = listPos.begin(); itr != listPos.end(); itr++) {
		std::list<LaserNode>::iterator itrNext = std::next(itr);
		if (itrNext == listPos.end()) break;

			D3DXVECTOR2* pos = &itr->pos;
			D3DXVECTOR2* posNext = &itrNext->pos;
			float nodeDist = hypotf(posNext->x - pos->x, posNext->y - pos->y);
			lengthAcc += nodeDist;

			float createDist = 0U;
			while (lengthAcc >= itemDistance_) {
				float lerpMul = (itemDistance_ >= nodeDist) ? 0.0 : (createDist / nodeDist);
				_RequestItem(Math::Lerp::Linear(pos->x, posNext->x, lerpMul),
					Math::Lerp::Linear(pos->y, posNext->y, lerpMul));
				createDist += itemDistance_;
				lengthAcc -= itemDistance_;
			}
		}
	}
}

//****************************************************************************
//StgShotPatternGeneratorObject (ECL-style bullets firing)
//****************************************************************************
StgShotPatternGeneratorObject::StgShotPatternGeneratorObject(StgStageController* stageController) : StgShotObject(stageController) {
	typeObject_ = TypeObject::ShotPattern;
	bAutoDeletePattern_ = false;

	typePattern_ = PATTERN_TYPE_FAN;
	typeShot_ = TypeObject::Shot;

	shotWay_ = 1U;
	shotStack_ = 1U;
	shotCutoff_ = 1U;
	bInterlace_ = false;

	wayScale_ = 1;
	stackScale_ = 1;
	lerpScale_ = Math::Lerp::GetFunc<float, float>(Math::Lerp::LINEAR);

	wayWait_ = 0;
	stackWait_ = 0;
	shotsWaiting_.clear();

	repeatNext_ = 0;
	repeatWait_ = 0;
	repeatTimes_ = 0;
	fireCallback_.clear();

	basePointX_ = BASEPOINT_RESET;
	basePointY_ = BASEPOINT_RESET;

	basePointOffsetX_ = 0;
	basePointOffsetY_ = 0;

	fireRadiusOffset_ = 0;
	fireRadiusScale_ = 0;
	lerpRadius_ = Math::Lerp::GetFunc<float, float>(Math::Lerp::LINEAR);

	speedBase_ = 1;
	speedArgument_ = 1;
	speedOff_ = 0;
	lerpSpeed_ = Math::Lerp::GetFunc<double, double>(Math::Lerp::LINEAR);

	angleBase_ = 0;
	angleArgument_ = 0;
	angleOff_ = 0;

	angularVelocity_ = 0;
	bFixedAngle_ = false;

	laserWidth_ = 16;
	laserLength_ = 64;
}

void StgShotPatternGeneratorObject::CleanUp() {
	if (parent_ == nullptr && bAutoDeletePattern_) {
		auto objectManager = stageController_->GetMainObjectManager();
		objectManager->DeleteObject(this);
	}
}

void StgShotPatternGeneratorObject::Clone(DxScriptObjectBase* _src, bool deepCopy) {
	StgShotObject::Clone(_src, deepCopy);

	auto src = (StgShotPatternGeneratorObject*)_src;

	parent_ = src->parent_;
	shotParent_ = src->shotParent_;

	bAutoDeletePattern_ = src->bAutoDeletePattern_;

	typeShot_ = src->typeShot_;
	typePattern_ = src->typePattern_;

	shotWay_ = src->shotWay_;
	shotStack_ = src->shotStack_;
	shotCutoff_ = src->shotCutoff_;
	bInterlace_ = src->bInterlace_;

	wayScale_ = src->wayScale_;
	stackScale_ = src->stackScale_;
	lerpScale_ = src->lerpScale_;

	wayWait_ = src->wayWait_;
	stackWait_ = src->stackWait_;
	// shotsWaiting_ = src->shotsWaiting_; purposefully omitted

	repeatNext_ = src->repeatNext_;
	repeatWait_ = src->repeatWait_;
	repeatTimes_ = src->repeatTimes_;
	fireCallback_ = src->fireCallback_;

	basePointX_ = src->basePointX_;
	basePointY_ = src->basePointY_;

	basePointOffsetX_ = src->basePointOffsetX_;
	basePointOffsetY_ = src->basePointOffsetY_;

	fireRadiusOffset_ = src->fireRadiusOffset_;
	fireRadiusScale_ = src->fireRadiusScale_;
	lerpRadius_ = src->lerpRadius_;

	speedBase_ = src->speedBase_;
	speedArgument_ = src->speedArgument_;
	speedOff_ = src->speedOff_;
	lerpSpeed_ = src->lerpSpeed_;

	angleBase_ = src->angleBase_;
	angleArgument_ = src->angleArgument_;
	angleOff_ = src->angleOff_;

	angularVelocity_ = src->angularVelocity_;
	bFixedAngle_ = src->bFixedAngle_;

	laserWidth_ = src->laserWidth_;
	laserLength_ = src->laserLength_;
}

void StgShotPatternGeneratorObject::Work() {
	if (repeatTimes_ != 0 && repeatNext_ <= frameExist_) {
		repeatNext_ = repeatWait_ + frameExist_;
		repeatTimes_--;

		if (!fireCallback_.empty()) {
			std::vector<int> res;
			FireSet(scriptData_, controller_, &res);
			for (auto& callback : fireCallback_) {
				if (std::get<2>(callback) == NULL) continue;
				script_block* subIvk = (script_block*)(std::get<2>(callback) & 0xffffffff);
				script_machine* machine = (script_machine*)std::get<0>(callback);

				ptrdiff_t threadAdvance = std::get<1>(callback) - std::distance(machine->threads.begin(), machine->current_thread_index);
				std::advance(machine->current_thread_index, threadAdvance);

				script_machine::environment* e = machine->add_child_block(subIvk);
				DxScript* script = (DxScript*)machine->data;
				e->stack.push_back(script->CreateIntArrayValue(res));
				e->stack.push_back(script->CreateIntValue(idObject_));

				std::advance(machine->current_thread_index, -threadAdvance);
			}
		}
		else
			FireSet(scriptData_, controller_, nullptr);
	}

	size_t sz = shotsWaiting_.size();
	for (size_t i = 0U; i < sz; ) {
		if (std::get<0>(shotsWaiting_[i]) <= frameExist_) {
			ref_unsync_ptr<StgShotObject> objShot = std::get<1>(shotsWaiting_[i]);

			objShot->SetEnableMovement(bEnableMovement_);
			std::get<3>(shotsWaiting_[i])->AddShot(objShot);

			ref_unsync_weak_ptr<StgMoveParent> shotParent = std::get<2>(shotsWaiting_[i]);
			if (shotParent) shotParent->AddChild(shotParent, objShot);

			shotsWaiting_.erase(shotsWaiting_.begin() + i);
			--sz;
		}
		else {
			++i;
		}
	}
}

void StgShotPatternGeneratorObject::FireSet(void* scriptData, StgStageController* controller, std::vector<int>* idVector) {
	if (idVector) idVector->clear();

	StgStageScript* script = (StgStageScript*)scriptData;

	ref_unsync_ptr<StgPlayerObject> objPlayer = controller->GetPlayerObject();
	auto objManager = controller->GetMainObjectManager();
	StgShotManager* shotManager = controller->GetShotManager();

	shared_ptr<RandProvider> randGenerator = controller->GetStageInformation()->GetRandProvider();

	if (idShotData_ < 0) return;
	if (shotWay_ == 0U || shotStack_ == 0U) return;

	float basePosX = basePointX_;
	float basePosY = basePointY_;
	if (!parent_.expired()) {
		if (basePointX_ == BASEPOINT_RESET)
			basePosX = parent_->GetPositionX();
		if (basePointY_ == BASEPOINT_RESET)
			basePosY = parent_->GetPositionY();
	}
	basePosX += basePointOffsetX_;
	basePosY += basePointOffsetY_;

	auto __CreateShot = [&](float _x, float _y, double _ss, double _sa, double _wayWaitScale, double _stackWaitScale,
		size_t _iWay, size_t _iStack, size_t _wayStart, size_t _wayEnd, size_t _stackEnd) -> bool {
		if (shotManager->GetShotCountAll() >= StgShotManager::SHOT_MAX) return false;

		ref_unsync_ptr<StgShotObject> objShot;
		switch (typeShot_) {
		case TypeObject::Shot:
		{
			ref_unsync_ptr<StgNormalShotObject> ptrShot(new StgNormalShotObject(controller));
			ptrShot->SetGraphicAngularVelocity(angularVelocity_);
			ptrShot->SetFixedAngle(bFixedAngle_);
			objShot = ptrShot;
			break;
		}
		case TypeObject::LooseLaser:
		{
			ref_unsync_ptr<StgLooseLaserObject> ptrShot(new StgLooseLaserObject(controller));
			ptrShot->SetLength(laserLength_);
			ptrShot->SetRenderWidth(laserWidth_);
			objShot = ptrShot;
			break;
		}
		case TypeObject::StraightLaser:
		{
			ref_unsync_ptr<StgStraightLaserObject> ptrShot(new StgStraightLaserObject(controller));
			ptrShot->SetLength(laserLength_);
			ptrShot->SetRenderWidth(laserWidth_);
			objShot = ptrShot;
			break;
		}
		case TypeObject::CurveLaser:
		{
			ref_unsync_ptr<StgCurveLaserObject> ptrShot(new StgCurveLaserObject(controller));
			ptrShot->SetLength(laserLength_);
			ptrShot->SetRenderWidth(laserWidth_);
			objShot = ptrShot;
			break;
		}
		}

		if (objShot == nullptr) return false;

		objShot->StgShotObject::Clone(this, false);
		objShot->SetX(_x);
		objShot->SetY(_y);
		objShot->SetSpeed(_ss);
		objShot->SetDirectionAngle(_sa);

		if (wayScale_ != 1 || stackScale_ != 1) {
			double scaling = lerpScale_(1, wayScale_, (double)_iWay / shotWay_)
				* lerpScale_(1, stackScale_, (double)_iStack / shotStack_);

			objShot->SetScale(objShot->GetScale() * scaling);

			D3DXVECTOR2 hitboxScale(objShot->GetHitboxScaleX() * scaling, objShot->GetHitboxScaleY() * scaling);
			objShot->SetHitboxScale(hitboxScale);

			D3DXVECTOR3* delayScale = objShot->GetDelayParameter()->GetScaleVector();
			delayScale->x *= scaling;
			delayScale->y *= scaling;
		}

		int idRes = script->AddObject(objShot);
		if (idRes == DxScript::ID_INVALID) return false;

		if (idVector) idVector->push_back(idRes);

		if (wayWait_ == 0 && stackWait_ == 0) {
			shotManager->AddShot(objShot);
			if (shotParent_) shotParent_->AddChild(shotParent_, objShot);
		}
		else {
			size_t wayWait = _wayWaitScale * ((wayWait_ > 0) ? (wayWait_ * (_iWay - _wayStart)) : (-wayWait_ * (_wayEnd - _iWay - 1)));
			size_t stackWait = _stackWaitScale * ((stackWait_ > 0) ? (stackWait_ * _iStack) : (-stackWait_ * (_stackEnd - _iStack - 1)));

			size_t totalWait = wayWait + stackWait;

			if (totalWait > 0) {
				objShot->SetEnableMovement(false);
				shotsWaiting_.push_back(std::tuple<size_t, ref_unsync_ptr<StgShotObject>, ref_unsync_weak_ptr<StgMoveParent>, StgShotManager*>
					(frameExist_ + totalWait, objShot, shotParent_, shotManager));
			}
			else {
				shotManager->AddShot(objShot);
				if (shotParent_) shotParent_->AddChild(shotParent_, objShot);
			}
		}

		return true;
	};

	{
		switch (typePattern_) {
		case PATTERN_TYPE_FAN:
		case PATTERN_TYPE_FAN_AIMED:
		{
			double wsw = bInterlace_ ? 0.5 : 1;

			double ini_angle = angleBase_;
			if (objPlayer != nullptr && typePattern_ == PATTERN_TYPE_FAN_AIMED)
				ini_angle += atan2(objPlayer->GetY() - basePosY, objPlayer->GetX() - basePosX);
			double ang_off_way = (double)(shotWay_ / 2) - (shotWay_ % 2 == 0 ? 0.5 : 0.0);

			size_t wayStart = (shotCutoff_ < 0) ? (shotWay_ + shotCutoff_) : 0;
			size_t wayEnd = (shotCutoff_ >= 0) ? shotCutoff_ : shotWay_;

			size_t sendEnd = (bInterlace_ && shotWay_ % 2 == 0) ? (wayEnd - 1) : wayEnd;

			for (size_t iWay = wayStart; iWay < wayEnd; ++iWay) {
				size_t iw = bInterlace_ ? Math::Interlace(iWay, shotWay_) : iWay;
				double sa = ini_angle + (iw - ang_off_way) * angleArgument_;
				double r_fac[2] = { cos(sa), sin(sa) };
				sa += angleOff_;

				size_t vee = Math::Vee(iw, shotWay_);
				double lerp = bInterlace_ ? ((double)vee * 2 / shotWay_) : ((double)iWay / shotWay_);
				double _ss = speedOff_ * lerpSpeed_(0, 1, lerp) + 1;
				float rad = fireRadiusOffset_ * (fireRadiusScale_ * lerpRadius_(0, 1, lerp) + 1);

				size_t ws = bInterlace_ ? (vee * 2) : iw;

				for (size_t iStack = 0; iStack < shotStack_; ++iStack) {
					double ss = speedBase_;
					if (shotStack_ > 1) {
						ss += (speedArgument_ - speedBase_) * (iStack / ((double)shotStack_ - 1));
					}
					ss *= _ss;

					float sx = basePosX + rad * r_fac[0];
					float sy = basePosY + rad * r_fac[1];

					__CreateShot(sx, sy, ss, sa, wsw, 1, ws, iStack, wayStart, sendEnd, shotStack_);
				}
			}
			break;
		}
		case PATTERN_TYPE_RING:
		case PATTERN_TYPE_RING_AIMED:
		{
			double wsw = bInterlace_ ? 0.5 : 1;

			double ini_angle = angleBase_;
			if (objPlayer != nullptr && typePattern_ == PATTERN_TYPE_RING_AIMED)
				ini_angle += atan2(objPlayer->GetY() - basePosY, objPlayer->GetX() - basePosX);

			size_t wayStart = (shotCutoff_ < 0) ? (shotWay_ + shotCutoff_) : 0;
			size_t wayEnd = (shotCutoff_ >= 0) ? shotCutoff_ : shotWay_;

			size_t sendEnd = (bInterlace_ && shotWay_ % 2 == 0) ? (wayEnd + 1) : wayEnd;

			for (size_t iWay = wayStart; iWay < wayEnd; ++iWay) {
				size_t iw = bInterlace_ ? Math::Interlace(shotWay_ - iWay - 1, shotWay_) : iWay;
				double sa_b = ini_angle + (GM_PI_X2 / (double)shotWay_) * iw;

				size_t vee = Math::Vee(iw, shotWay_ + 1);
				double lerp = bInterlace_ ? ((double)vee * 2 / shotWay_) : ((double)iWay / shotWay_);
				double _ss = speedOff_ * lerpSpeed_(0, 1, lerp) + 1;
				float rad = fireRadiusOffset_ * (fireRadiusScale_ * lerpRadius_(0, 1, lerp) + 1);

				size_t ws = bInterlace_ ? (vee * 2) : iw;

				for (size_t iStack = 0; iStack < shotStack_; ++iStack) {
					double ss = speedBase_;
					if (shotStack_ > 1) ss += (speedArgument_ - speedBase_) * (iStack / ((double)shotStack_ - 1));
					ss *= _ss;

					double sa = sa_b + iStack * angleArgument_;
					float sx = basePosX + rad * cos(sa);
					float sy = basePosY + rad * sin(sa);
					sa += angleOff_;

					__CreateShot(sx, sy, ss, sa, wsw, 1, ws, iStack, wayStart, sendEnd, shotStack_);
				}
			}
			break;
		}
		case PATTERN_TYPE_ARROW:
		case PATTERN_TYPE_ARROW_AIMED:
		{
			double wsw = bInterlace_ ? 0.5 : 1;

			double ini_angle = angleBase_;
			if (objPlayer != nullptr && typePattern_ == PATTERN_TYPE_ARROW_AIMED)
				ini_angle += atan2(objPlayer->GetY() - basePosY, objPlayer->GetX() - basePosX);

			size_t wayStart = (shotCutoff_ < 0) ? (shotWay_ + shotCutoff_) : 0;
			size_t wayEnd = (shotCutoff_ >= 0) ? shotCutoff_ : shotWay_;

			size_t sendEnd = (bInterlace_ && shotWay_ % 2 == 0) ? (wayEnd + 1) : wayEnd;

			for (size_t iWay = wayStart; iWay < wayEnd; ++iWay) {
				size_t iw = bInterlace_ ? Math::Interlace(shotWay_ - iWay - 1, shotWay_) : iWay;
				double w_angle = ini_angle + (GM_PI_X2 / (double)shotWay_) * iw;

				size_t vee = Math::Vee(iw, shotWay_ + 1);
				double lerp = bInterlace_ ? ((double)vee * 2 / shotWay_) : ((double)iWay / shotWay_);
				double _ss = speedOff_ * lerpSpeed_(0, 1, lerp) + 1;
				float rad = fireRadiusOffset_ * (fireRadiusScale_ * lerpRadius_(0, 1, lerp) + 1);

				size_t ws = bInterlace_ ? (vee * 2) : iw;

				for (size_t iStack = 0U; iStack < shotStack_; ++iStack) {
					size_t ist = bInterlace_ ? (shotStack_ - iStack - 1) : iStack;

					int side = (-1 + 2 * (ist % 2));
					size_t advance = (shotStack_ % 2 == 0) ? (ist / 2) : ((ist + 1) / 2);

					double ss = _ss * (speedBase_ + (speedArgument_ - speedBase_) * (double)advance / 2.0);
					double sa = w_angle;
					if (shotStack_ % 2 == 0)
						sa += (angleArgument_ / 2.0) * side;
					sa += angleArgument_ * advance * side;
					float sx = basePosX + rad * cos(sa);
					float sy = basePosY + rad * sin(sa);

					if (angleOff_ != 0) {
						float ox = basePosX + rad * cos(w_angle);
						float oy = basePosY + rad * sin(w_angle);
						double rpos[2] = { sx, sy };
						Math::Rotate2D(rpos, angleOff_, ox, oy);
						sx = rpos[0];
						sy = rpos[1];
						sa += angleOff_;
					}

					__CreateShot(sx, sy, ss, sa, wsw, 0.5, ws, advance * 2, wayStart, sendEnd, shotStack_);
				}
			}
			break;
		}
		case PATTERN_TYPE_POLYGON:
		case PATTERN_TYPE_POLYGON_AIMED:
		{
			double ini_angle = angleBase_;
			if (objPlayer != nullptr && typePattern_ == PATTERN_TYPE_POLYGON_AIMED)
				ini_angle += atan2(objPlayer->GetY() - basePosY, objPlayer->GetX() - basePosX);

			size_t numEdges = shotWay_;
			size_t numShotPerEdge = shotStack_;
			int edgeSkip = std::round(Math::RadianToDegree(angleArgument_));

			double r_fac[2] = { cos(ini_angle), sin(ini_angle) };

			size_t wayStart = (shotCutoff_ < 0) ? (numEdges + shotCutoff_) : 0;
			size_t wayEnd = (shotCutoff_ >= 0) ? shotCutoff_ : numEdges;

			size_t sendEnd = (bInterlace_ && shotWay_ % 2 == 0) ? (wayEnd + 1) : wayEnd;

			for (size_t iEdge = wayStart; iEdge < wayEnd; ++iEdge) {
				double from_ang = (GM_PI_X2 / numEdges) * (double)iEdge;
				double to_ang = (GM_PI_X2 / numEdges) * (double)((int)iEdge + edgeSkip);
				double from_pos[2] = { cos(from_ang), sin(from_ang) };
				double to_pos[2] = { cos(to_ang), sin(to_ang) };

				for (size_t iShot = 0; iShot < numShotPerEdge; ++iShot) {
					//Will always be just a little short of a full 1, intentional.
					double rate = iShot / (double)numShotPerEdge;

					double _sx_b = Math::Lerp::Linear(from_pos[0], to_pos[0], rate);
					double _sy_b = Math::Lerp::Linear(from_pos[1], to_pos[1], rate);
					double _sx = _sx_b * r_fac[0] + _sy_b * r_fac[1];
					double _sy = _sx_b * r_fac[1] - _sy_b * r_fac[0];

					size_t vee = Math::Vee(iShot, numShotPerEdge + 1);
					double lerp = bInterlace_ ? ((double)vee * 2 / numShotPerEdge) : ((double)iShot / numShotPerEdge);
					float rad = fireRadiusOffset_ * (fireRadiusScale_ * lerpRadius_(0, 1, lerp) + 1);
					float sx = basePosX + rad * _sx;
					float sy = basePosY + rad * _sy;

					double sa = atan2(_sy, _sx) + angleOff_;
					double ss = hypot(_sx, _sy) * speedBase_;
					ss *= speedOff_ * lerpSpeed_(0, 1, lerp) + 1;

					size_t ws = bInterlace_ ? (vee * 2) : iShot;

					__CreateShot(sx, sy, ss, sa, 1, 1, iEdge, ws, wayStart, sendEnd, shotStack_);
				}
			}
			break;
		}
		case PATTERN_TYPE_ELLIPSE:
		case PATTERN_TYPE_ELLIPSE_AIMED:
		{
			double wsw = bInterlace_ ? 0.5 : 1;

			double el_pointing_angle = angleBase_;
			if (objPlayer != nullptr && typePattern_ == PATTERN_TYPE_ELLIPSE_AIMED)
				el_pointing_angle += atan2(objPlayer->GetY() - basePosY, objPlayer->GetX() - basePosX);

			double r_eccentricity = speedArgument_ / speedBase_;

			size_t wayStart = (shotCutoff_ < 0) ? (shotWay_ + shotCutoff_) : 0;
			size_t wayEnd = (shotCutoff_ >= 0) ? shotCutoff_ : shotWay_;

			size_t sendEnd = (bInterlace_ && shotWay_ % 2 == 0) ? (wayEnd + 1) : wayEnd;

			for (size_t iWay = wayStart; iWay < wayEnd; ++iWay) {
				size_t iw = bInterlace_ ? Math::Interlace(shotWay_ - iWay - 1, shotWay_) : iWay;
				double angle_cur = GM_PI_X2 / (double)shotWay_ * iw + angleArgument_;

				double rpos[2] = { 1 * cos(angle_cur), r_eccentricity * sin(angle_cur) };
				Math::Rotate2D(rpos, el_pointing_angle, 0, 0);

				size_t vee = Math::Vee(iw, shotWay_ + 1);
				double lerp = bInterlace_ ? ((double)vee * 2 / shotWay_) : ((double)iWay / shotWay_);
				float rad = fireRadiusOffset_ * (fireRadiusScale_ * lerpRadius_(0, 1, lerp) + 1);
				double sa = atan2(rpos[1], rpos[0]) + angleOff_;
				double sv = speedOff_ * lerpSpeed_(0, 1, lerp) + 1;
				double ss = hypot(rpos[0], rpos[1]) * speedBase_ * sv;
				float sx = basePosX + rad * rpos[0];
				float sy = basePosY + rad * rpos[1];

				size_t ws = bInterlace_ ? (vee * 2) : iw;

				__CreateShot(sx, sy, ss, sa, wsw, 1, ws, 0, wayStart, sendEnd, shotStack_);
			}
			break;
		}
		case PATTERN_TYPE_SCATTER_ANGLE:
		case PATTERN_TYPE_SCATTER_SPEED:
		case PATTERN_TYPE_SCATTER:
		{
			double ini_angle = angleBase_;

			size_t wayStart = (shotCutoff_ < 0) ? (shotWay_ + shotCutoff_) : 0;
			size_t wayEnd = (shotCutoff_ >= 0) ? shotCutoff_ : shotWay_;

			std::vector<double> vss((wayEnd - wayStart) * shotStack_);

			for (size_t iWay = wayStart; iWay < wayEnd; ++iWay) {
				for (size_t iStack = 0; iStack < shotStack_; ++iStack) {
					vss[iWay * shotStack_ + iStack] = speedBase_ + (speedArgument_ - speedBase_) *
						((shotStack_ > 1 && typePattern_ == PATTERN_TYPE_SCATTER_ANGLE) ?
							(iStack / ((double)shotStack_ - 1)) : randGenerator->GetReal());
				}
			}

			if (bInterlace_)
				std::sort(vss.begin(), vss.end());

			for (size_t iWay = wayStart; iWay < wayEnd; ++iWay) {
				for (size_t iStack = 0; iStack < shotStack_; ++iStack) {
					double ss = vss[iWay * shotStack_ + iStack];
					double sa = ini_angle + ((typePattern_ == PATTERN_TYPE_SCATTER_SPEED) ?
						(GM_PI_X2 / (double)shotWay_ * iWay) + angleArgument_ * iStack :
						randGenerator->GetReal(-angleArgument_, angleArgument_));

					float lerp = std::abs<double>(Math::AngleDifferenceRad(ini_angle, sa)) / GM_PI;
					float rad = fireRadiusOffset_ * (fireRadiusScale_ * lerpRadius_(0, 1, lerp) + 1);
					ss *= speedOff_ * lerpSpeed_(0, 1, lerp) + 1;
					float sx = basePosX + rad * cos(sa);
					float sy = basePosY + rad * sin(sa);
					sa += angleOff_;

					size_t ws = bInterlace_ ? (lerp * shotWay_) : iWay;

					__CreateShot(sx, sy, ss, sa, 1, 1, ws, iStack, wayStart, wayEnd, shotStack_);
				}
			}
			break;
		}
		case PATTERN_TYPE_LINE:
		case PATTERN_TYPE_LINE_AIMED:
		{
			double wsw = bInterlace_ ? 0.5 : 1;

			double ini_angle = angleBase_;
			double angle_off = angleArgument_ / 2;
			if (objPlayer != nullptr && typePattern_ == PATTERN_TYPE_LINE_AIMED)
				ini_angle += atan2(objPlayer->GetY() - basePosY, objPlayer->GetX() - basePosX);

			double from_ang = ini_angle + angle_off;
			double to_ang = ini_angle - angle_off;

			double from_pos[2] = { cos(from_ang), sin(from_ang) };
			double to_pos[2] = { cos(to_ang), sin(to_ang) };

			size_t wayStart = (shotCutoff_ < 0) ? (shotWay_ + shotCutoff_) : 0;
			size_t wayEnd = (shotCutoff_ >= 0) ? shotCutoff_ : shotWay_;

			size_t sendEnd = (bInterlace_ && shotWay_ % 2 == 0) ? (wayEnd + 1) : wayEnd;

			for (size_t iWay = wayStart; iWay < wayEnd; ++iWay) {
				size_t iw = bInterlace_ ? (shotWay_ - iWay - 1) : iWay;
				//Will always be just a little short of a full 1, intentional.
				double rate = shotWay_ > 1 ? (iw / ((double)shotWay_ - 1)) : 0.5;

				double _sx = Math::Lerp::Linear(from_pos[0], to_pos[0], rate);
				double _sy = Math::Lerp::Linear(from_pos[1], to_pos[1], rate);
				size_t vee = Math::Vee(iw, shotWay_);
				double lerp = bInterlace_ ? ((double)vee * 2 / shotWay_) : ((double)iWay / shotWay_);
				float rad = fireRadiusOffset_ * (fireRadiusScale_ * lerpRadius_(0, 1, lerp) + 1);
				float sx = basePosX + rad * _sx;
				float sy = basePosY + rad * _sy;
				double sa = atan2(_sy, _sx) + angleOff_;
				double _ss = hypot(_sx, _sy) * (speedOff_ * lerpSpeed_(0, 1, lerp) + 1);

				size_t ws = bInterlace_ ? (vee * 2) : iw;

				for (size_t iStack = 0; iStack < shotStack_; ++iStack) {
					double ss = speedBase_;
					if (shotStack_ > 1) ss += (speedArgument_ - speedBase_) * (iStack / ((double)shotStack_ - 1));

					__CreateShot(sx, sy, ss * _ss, sa, wsw, 1, ws, iStack, wayStart, sendEnd, shotStack_);
				}
			}
			break;
		}
		case PATTERN_TYPE_ROSE:
		case PATTERN_TYPE_ROSE_AIMED:
		{
			double wsw = bInterlace_ ? 0.5 : 1;

			double ini_angle = angleBase_;
			if (objPlayer != nullptr && typePattern_ == PATTERN_TYPE_ROSE_AIMED)
				ini_angle += atan2(objPlayer->GetY() - basePosY, objPlayer->GetX() - basePosX);

			size_t numPetal = shotWay_;
			size_t numShotPerPetal = shotStack_;
			int petalSkip = std::round(Math::RadianToDegree(angleArgument_));

			double petalGap = GM_PI_X2 / numPetal;
			double angGap = (GM_PI_X2 / (numPetal * numShotPerPetal) * petalSkip);

			size_t wayStart = (shotCutoff_ < 0) ? (numPetal + shotCutoff_) : 0;
			size_t wayEnd = (shotCutoff_ >= 0) ? shotCutoff_ : numPetal;

			size_t sendEnd = (bInterlace_ && shotWay_ % 2 == 0) ? (wayEnd + 1) : wayEnd;

			for (size_t iStack = 0; iStack < numShotPerPetal; ++iStack) {
				size_t ist = bInterlace_ ? (shotStack_ - iStack - 1) : iStack;
				double _ss = speedBase_ + (speedArgument_ - speedBase_) * sin(GM_PI / numShotPerPetal * ist);

				for (size_t iShot = wayStart; iShot < wayEnd; ++iShot) {
					size_t vee = Math::Vee(iShot, numPetal + 1);
					double lerp = bInterlace_ ? ((double)vee * 2 / numPetal) : ((double)iShot / numPetal);
					float rad = fireRadiusOffset_ * (fireRadiusScale_ * lerpRadius_(0, 1, lerp) + 1);
					double ss = _ss * (speedOff_ * lerpSpeed_(0, 1, lerp) + 1);
					double sa = ini_angle + iShot * petalGap + ist * angGap;
					float sx = basePosX + rad * cos(sa);
					float sy = basePosY + rad * sin(sa);
					sa += angleOff_;

					size_t ws = bInterlace_ ? (vee * 2) : iShot;

					__CreateShot(sx, sy, ss, sa, wsw, 1, ws, iStack, wayStart, sendEnd, shotStack_);
				}
			}
			break;
		}
		}
	}
}

void StgShotPatternGeneratorObject::ClearWaiting() {
	auto objectManager = stageController_->GetMainObjectManager();

	for (auto& shot : shotsWaiting_)
		objectManager->DeleteObject(std::get<1>(shot));

	shotsWaiting_.clear();

	repeatNext_ = 0;
	repeatWait_ = 0;
	repeatTimes_ = 0;
	fireCallback_.clear();
}