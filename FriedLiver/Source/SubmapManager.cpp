
#include "stdafx.h"
#include "SiftGPU/SiftGPU.h"
#include "SiftGPU/SiftMatch.h"
#include "SiftGPU/MatrixConversion.h"
#include "SiftGPU/SIFTMatchFilter.h"
#include "GlobalAppState.h"
#include "SiftVisualization.h"
#include "ConditionManager.h"

#include "SubmapManager.h"

#ifdef EVALUATE_SPARSE_CORRESPONDENCES
#include "SensorDataReader.h"
#include "FrameProjection.h"
#endif

SubmapManager::SubmapManager()
{
	m_sift = NULL;
	m_siftMatcher = NULL;

	m_currentLocal = NULL;
	m_nextLocal = NULL;
	m_optLocal = NULL;
	m_global = NULL;
	m_numTotalFrames = 0;
	m_submapSize = 0;

	m_currentLocalCache = NULL;
	m_nextLocalCache = NULL;
	m_globalCache = NULL;
	m_optLocalCache = NULL;
	//m_globalTimer = NULL;

	d_globalTrajectory = NULL;
	d_completeTrajectory = NULL;
	d_localTrajectories = NULL;

	d_siftTrajectory = NULL;

	m_continueRetry = 0;
	m_revalidatedIdx = (unsigned int)-1;
#ifdef DEBUG_PRINT_MATCHING
	_debugPrintMatches = false;
#endif
#ifdef EVALUATE_SPARSE_CORRESPONDENCES
	//_siftMatch_frameFrameLocal = vec2ui(0, 0);
	//_siftVerify_frameFrameLocal = vec2ui(0, 0);
	_siftRaw_frameFrameGlobal = vec2ui(0, 0);
	_siftMatch_frameFrameGlobal = vec2ui(0, 0);
	_siftVerify_frameFrameGlobal = vec2ui(0, 0);
	_opt_frameFrameGlobal = vec2ui(0, 0);
#endif
}

void SubmapManager::initSIFT(unsigned int widthSift, unsigned int heightSift)
{
	m_sift = new SiftGPU;
	m_siftMatcher = new SiftMatchGPU(GlobalBundlingState::get().s_maxNumKeysPerImage);

	m_sift->SetParams(widthSift, heightSift, false, 150, GlobalAppState::get().s_sensorDepthMin, GlobalAppState::get().s_sensorDepthMax);
	m_sift->InitSiftGPU();
	m_siftMatcher->InitSiftMatch();
}

void SubmapManager::init(unsigned int maxNumGlobalImages, unsigned int maxNumLocalImages, unsigned int maxNumKeysPerImage, unsigned int submapSize,
	const CUDAImageManager* manager, unsigned int numTotalFrames /*= (unsigned int)-1*/)
{
	initSIFT(GlobalBundlingState::get().s_widthSIFT, GlobalBundlingState::get().s_heightSIFT);
	const unsigned int maxNumImages = GlobalBundlingState::get().s_maxNumImages;
	const unsigned int maxNumResiduals = MAX_MATCHES_PER_IMAGE_PAIR_FILTERED * (maxNumImages*(maxNumImages - 1)) / 2;
	m_SparseBundler.init(GlobalBundlingState::get().s_maxNumImages, maxNumResiduals);

	// cache
	const unsigned int cacheInputWidth = manager->getSIFTDepthWidth();
	const unsigned int cacheInputHeight = manager->getSIFTDepthHeight();
	const unsigned int downSampWidth = GlobalBundlingState::get().s_downsampledWidth;
	const unsigned int downSampHeight = GlobalBundlingState::get().s_downsampledHeight;

	const mat4f inputIntrinsics = manager->getSIFTDepthIntrinsics();
	m_currentLocalCache = new CUDACache(cacheInputWidth, cacheInputHeight, downSampWidth, downSampHeight, maxNumLocalImages, inputIntrinsics);
	m_nextLocalCache = new CUDACache(cacheInputWidth, cacheInputHeight, downSampWidth, downSampHeight, maxNumLocalImages, inputIntrinsics);
	m_optLocalCache = new CUDACache(cacheInputWidth, cacheInputHeight, downSampWidth, downSampHeight, maxNumLocalImages, inputIntrinsics);
	m_globalCache = new CUDACache(cacheInputWidth, cacheInputHeight, downSampWidth, downSampHeight, maxNumGlobalImages, inputIntrinsics);

	m_numTotalFrames = numTotalFrames;
	m_submapSize = submapSize;

	// sift manager
	m_currentLocal = new SIFTImageManager(m_submapSize, maxNumLocalImages, maxNumKeysPerImage);
	m_nextLocal = new SIFTImageManager(m_submapSize, maxNumLocalImages, maxNumKeysPerImage);
	m_optLocal = new SIFTImageManager(m_submapSize, maxNumLocalImages, maxNumKeysPerImage);
	m_global = new SIFTImageManager(m_submapSize, maxNumGlobalImages, maxNumKeysPerImage);

	m_invalidImagesList.resize(maxNumGlobalImages * m_submapSize, 1);

	MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_globalTrajectory, sizeof(float4x4)*maxNumGlobalImages));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_completeTrajectory, sizeof(float4x4)*maxNumGlobalImages*m_submapSize));
	MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_localTrajectories, sizeof(float4x4)*maxNumLocalImages*maxNumGlobalImages));
	m_localTrajectoriesValid.resize(maxNumGlobalImages);

	float4x4 id;	id.setIdentity();
	MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_globalTrajectory, &id, sizeof(float4x4), cudaMemcpyHostToDevice)); // set first to identity
	std::vector<mat4f> initialLocalTrajectories(maxNumLocalImages * maxNumGlobalImages, mat4f::identity());
	MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_localTrajectories, initialLocalTrajectories.data(), sizeof(float4x4) * initialLocalTrajectories.size(), cudaMemcpyHostToDevice));

	MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_siftTrajectory, sizeof(float4x4)*maxNumGlobalImages*m_submapSize));
	MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_siftTrajectory, &id, sizeof(float4x4), cudaMemcpyHostToDevice)); // set first to identity

	MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_currIntegrateTransform, sizeof(float4x4)*maxNumGlobalImages*m_submapSize));
	MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_currIntegrateTransform, &id, sizeof(float4x4), cudaMemcpyHostToDevice)); // set first to identity
	m_currIntegrateTransform.resize(maxNumGlobalImages*m_submapSize);
	m_currIntegrateTransform[0].setIdentity();

	MLIB_CUDA_SAFE_CALL(cudaMalloc(&d_imageInvalidateList, sizeof(int) * maxNumGlobalImages * maxNumLocalImages));

	m_lastValidLocal = (unsigned int)-1; m_prevLastValidLocal = (unsigned int)-1;

#ifdef EVALUATE_SPARSE_CORRESPONDENCES
	const std::string prefix0 = "F:/Work/FriedLiver/data/iclnuim/Frame2FrameCorrespondences/NEW_OUTPUT/30percent/";
	const std::string prefix1 = "F:/Work/FriedLiver/data/iclnuim/Frame2FrameCorrespondences/output/30percent/";
	std::string name = util::removeExtensions(util::fileNameFromPath(GlobalAppState::get().s_binaryDumpSensorFile)) + ".global";
	if (util::fileExists(prefix0 + name))	name = prefix0 + name;
	else									name = prefix1 + name;
	std::cout << "loading gt from " << name << std::endl;
	MLIB_ASSERT(util::fileExists(name));
	BinaryDataStreamFile sGlobal(name, false);
	sGlobal >> _gtFrameFrameTransformsGlobal;
	sGlobal.closeStream();
#endif
}

SubmapManager::~SubmapManager()
{
	SAFE_DELETE(m_sift);
	SAFE_DELETE(m_siftMatcher);

	SAFE_DELETE(m_currentLocal);
	SAFE_DELETE(m_nextLocal);
	SAFE_DELETE(m_optLocal);
	SAFE_DELETE(m_global);

	SAFE_DELETE(m_currentLocalCache);
	SAFE_DELETE(m_nextLocalCache);
	SAFE_DELETE(m_optLocalCache);
	SAFE_DELETE(m_globalCache);

	MLIB_CUDA_SAFE_FREE(d_globalTrajectory);
	MLIB_CUDA_SAFE_FREE(d_completeTrajectory);
	MLIB_CUDA_SAFE_FREE(d_localTrajectories);

	MLIB_CUDA_SAFE_FREE(d_imageInvalidateList);
	MLIB_CUDA_SAFE_FREE(d_siftTrajectory);
	MLIB_CUDA_SAFE_FREE(d_currIntegrateTransform);
}

unsigned int SubmapManager::runSIFT(unsigned int curFrame, float* d_intensitySIFT, const float* d_inputDepthFilt, unsigned int depthWidth, unsigned int depthHeight, const uchar4* d_inputColor, unsigned int colorWidth, unsigned int colorHeight, const float* d_inputDepthRaw)
{
	SIFTImageGPU& curImage = m_currentLocal->createSIFTImageGPU();
	int success = m_sift->RunSIFT(d_intensitySIFT, d_inputDepthFilt);
	if (!success) throw MLIB_EXCEPTION("Error running SIFT detection");
	unsigned int numKeypoints = m_sift->GetKeyPointsAndDescriptorsCUDA(curImage, d_inputDepthFilt);
	//!!!debugging
	if (numKeypoints > GlobalBundlingState::get().s_maxNumKeysPerImage) throw MLIB_EXCEPTION("too many keypoints");
	//!!!debugging
	m_currentLocal->finalizeSIFTImageGPU(numKeypoints);

	// process cuda cache
	const unsigned int curLocalFrame = m_currentLocal->getCurrentFrame();
	m_currentLocalCache->storeFrame(d_inputDepthRaw, depthWidth, depthHeight, d_inputColor, colorWidth, colorHeight);

	// init next
	if (isLastLocalFrame(curFrame)) {
		mutex_nextLocal.lock();
		SIFTImageGPU& nextImage = m_nextLocal->createSIFTImageGPU();
		cutilSafeCall(cudaMemcpy(nextImage.d_keyPoints, curImage.d_keyPoints, sizeof(SIFTKeyPoint) * numKeypoints, cudaMemcpyDeviceToDevice));
		cutilSafeCall(cudaMemcpy(nextImage.d_keyPointDescs, curImage.d_keyPointDescs, sizeof(SIFTKeyPointDesc) * numKeypoints, cudaMemcpyDeviceToDevice));
		m_nextLocal->finalizeSIFTImageGPU(numKeypoints);
		m_nextLocalCache->copyCacheFrameFrom(m_currentLocalCache, curLocalFrame);
		mutex_nextLocal.unlock();
	}

	return curLocalFrame;
}

unsigned int SubmapManager::matchAndFilter(bool isLocal, SIFTImageManager* siftManager, CUDACache* cudaCache, const float4x4& siftIntrinsicsInv)
{
	const std::vector<int>& validImages = siftManager->getValidImages();
	Timer timer;

	// match with every other
	const unsigned int numFrames = siftManager->getNumImages();
	const unsigned int curFrame = siftManager->getCurrentFrame();
	const unsigned int startFrame = numFrames == curFrame + 1 ? 0 : curFrame + 1;
	if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.start(); }
	int num2 = (int)siftManager->getNumKeyPointsPerImage(curFrame);
	if (num2 == 0) return (unsigned int)-1;
	if (isLocal) m_mutexMatcher.lock();
	for (unsigned int prev = startFrame; prev < numFrames; prev++) {
		if (prev == curFrame) continue;
		uint2 keyPointOffset = make_uint2(0, 0);
		ImagePairMatch& imagePairMatch = siftManager->getImagePairMatch(prev, curFrame, keyPointOffset);

		SIFTImageGPU& image_i = siftManager->getImageGPU(prev);
		SIFTImageGPU& image_j = siftManager->getImageGPU(curFrame);
		int num1 = (int)siftManager->getNumKeyPointsPerImage(prev);

		if (validImages[prev] == 0 || num1 == 0 || num2 == 0) {
			unsigned int numMatch = 0;
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(imagePairMatch.d_numMatches, &numMatch, sizeof(unsigned int), cudaMemcpyHostToDevice));
		}
		else {
			if (!isLocal) m_mutexMatcher.lock();
			m_siftMatcher->SetDescriptors(0, num1, (unsigned char*)image_i.d_keyPointDescs);
			m_siftMatcher->SetDescriptors(1, num2, (unsigned char*)image_j.d_keyPointDescs);
			float ratioMax = isLocal ? GlobalBundlingState::get().s_siftMatchRatioMaxLocal : GlobalBundlingState::get().s_siftMatchRatioMaxGlobal;
			m_siftMatcher->GetSiftMatch(num1, imagePairMatch, keyPointOffset, GlobalBundlingState::get().s_siftMatchThresh, ratioMax);
			if (!isLocal) m_mutexMatcher.unlock();
		}
	}
	if (isLocal) m_mutexMatcher.unlock();
	if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.stop(); TimingLog::getFrameTiming(isLocal).timeSiftMatching = timer.getElapsedTimeMS(); }

	//bool lastValid = true;
	unsigned int lastMatchedFrame = (unsigned int)-1;
	if (curFrame > 0) { // can have a match to another frame

		// --- sort the current key point matches
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.start(); }
		siftManager->SortKeyPointMatchesCU(curFrame, startFrame, numFrames);

#ifdef DEBUG_PRINT_MATCHING
		const bool printDebug = _debugPrintMatches && !isLocal;
		const std::string suffix = isLocal ? "Local/" : "Global/";
		std::vector<unsigned int> _numRawMatches;
		if (printDebug) {
			siftManager->getNumRawMatchesDEBUG(_numRawMatches);
			SiftVisualization::printCurrentMatches("debug/rawMatches" + suffix, siftManager, cudaCache, false);
		}
#endif
#ifdef EVALUATE_SPARSE_CORRESPONDENCES
		const float minOverlapPercent = 0.3f;
		std::vector<SIFTKeyPoint> keyPoints;
		if (!isLocal) {
			siftManager->getSIFTKeyPointsDEBUG(keyPoints);
			std::vector<unsigned int> numRawMatches; siftManager->getNumRawMatchesDEBUG(numRawMatches);
			MLIB_ASSERT(!numRawMatches.empty());
			std::vector<uint2> keyPointIndices; std::vector<float> matchDistances;
			////begin determine if enough overlap
			std::vector<mat4f> estimatedTransforms(numRawMatches.size());
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(estimatedTransforms.data(), siftManager->getFiltTransformsDEBUG(), sizeof(mat4f)*estimatedTransforms.size(), cudaMemcpyDeviceToHost));
			//const std::vector<CUDACachedFrame>& cachedFrames = m_globalCache->getCacheFrames();
			//DepthImage32 depthImageCur(m_globalCache->getWidth(), m_globalCache->getHeight()); ColorImageR8G8B8 dummy;
			//MLIB_CUDA_SAFE_CALL(cudaMemcpy(depthImageCur.getData(), cachedFrames[curFrame].d_depthDownsampled, sizeof(float)*depthImageCur.getNumPixels(), cudaMemcpyDeviceToHost));
			////end determine if enough overlap
			//evaluate
			std::unordered_set<vec2ui> notEnoughOverlapSet;
			for (unsigned int f = startFrame; f < numRawMatches.size(); f++) {
				if (f == curFrame) continue;
				if (numRawMatches[f] >= 4) { //found a frame-frame match
					if (notEnoughOverlapSet.find(vec2ui(f, curFrame)) != notEnoughOverlapSet.end()) continue;
					siftManager->getRawKeyPointIndicesAndMatchDistancesDEBUG(f, keyPointIndices, matchDistances);
					//begin determine if enough overlap
					if (!FrameProjection::isApproxOverlapping(m_globalCache->getIntrinsics(), bbox2f(vec2f::origin, vec2f((float)m_globalCache->getWidth(), (float)m_globalCache->getHeight())),
						estimatedTransforms[f], mat4f::identity())) {
						notEnoughOverlapSet.insert(vec2ui(f, curFrame));
						continue;
					}
					//DepthImage32 depthImageOther(m_globalCache->getWidth(), m_globalCache->getHeight()); ColorImageR8G8B8 dummy;
					//MLIB_CUDA_SAFE_CALL(cudaMemcpy(depthImageOther.getData(), cachedFrames[f].d_depthDownsampled, sizeof(float)*depthImageOther.getNumPixels(), cudaMemcpyDeviceToHost));
					//vec2ui overlap = FrameProjection::computeOverlapBiDirectional(depthImageOther, dummy, depthImageCur, dummy, estimatedTransforms[f],
					//	m_globalCache->getIntrinsics(), 0.5f, 4.5f, 0.15f, 0.9f, 0.1f, minOverlapPercent, false);
					//float overlapPercent = (float)overlap.x / (float)overlap.y;
					//if (overlapPercent < minOverlapPercent) {
					//	notEnoughOverlapSet.insert(vec2ui(f, curFrame));
					//	continue;
					//}
					//end determine if enough overlap
					bool flip = false;
					vec2ui frameIndices(f, curFrame);
					if (f > curFrame) {
						frameIndices = vec2ui(curFrame, f);
						flip = true;
					}
					bool good = false; mat4f transform; //todo make sure the ones not in gt set are actually bad
					frameIndices *= m_submapSize;
					auto it = _gtFrameFrameTransformsGlobal.find(frameIndices);
					if (it != _gtFrameFrameTransformsGlobal.end()) {
						good = true;
						transform = it->second;
					}
					if (good) { //eval corrs 
						float meanErr = 0.0f;
						for (unsigned int k = 0; k < keyPointIndices.size(); k++) {
							const SIFTKeyPoint& ki = keyPoints[keyPointIndices[k].x]; const SIFTKeyPoint& kj = keyPoints[keyPointIndices[k].y];
							vec3f pos_i = MatrixConversion::toMlib(siftIntrinsicsInv) * (ki.depth * vec3f(ki.pos.x, ki.pos.y, 1.0f));
							vec3f pos_j = MatrixConversion::toMlib(siftIntrinsicsInv) * (kj.depth * vec3f(kj.pos.x, kj.pos.y, 1.0f));
							if (flip)	meanErr += (transform * pos_j - pos_i).lengthSq();
							else		meanErr += (transform * pos_i - pos_j).lengthSq();
						}
						meanErr /= (float)keyPointIndices.size();
						if (meanErr > 0.5f) good = false;
						if (good) _siftRaw_frameFrameGlobal.x++;
						_siftRaw_frameFrameGlobal.y++;
					}
					else {
						_siftRaw_frameFrameGlobal.y++;
					}
				}//sift matches found
			}
		}
#endif

		// --- filter matches
		const unsigned int minNumMatches = isLocal ? GlobalBundlingState::get().s_minNumMatchesLocal : GlobalBundlingState::get().s_minNumMatchesGlobal;
		//SIFTMatchFilter::ransacKeyPointMatches(siftManager, siftIntrinsicsInv, minNumMatches, GlobalBundlingState::get().s_maxKabschResidual2, false);
		//SIFTMatchFilter::filterKeyPointMatches(siftManager, siftIntrinsicsInv, minNumMatches);
		siftManager->FilterKeyPointMatchesCU(curFrame, startFrame, numFrames, siftIntrinsicsInv, minNumMatches, GlobalBundlingState::get().s_maxKabschResidual2);
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.stop(); TimingLog::getFrameTiming(isLocal).timeMatchFilterKeyPoint = timer.getElapsedTimeMS(); }

#ifdef DEBUG_PRINT_MATCHING
		std::vector<unsigned int> _numFiltMatches;
		if (printDebug) {
			siftManager->getNumFiltMatchesDEBUG(_numFiltMatches);
			SiftVisualization::printCurrentMatches("debug/matchesKeyFilt" + suffix, siftManager, cudaCache, true);
		}
#endif
#ifdef EVALUATE_SPARSE_CORRESPONDENCES
		if (!isLocal) {
			std::vector<unsigned int> numFiltMatches; siftManager->getNumFiltMatchesDEBUG(numFiltMatches);
			MLIB_ASSERT(!numFiltMatches.empty());
			std::vector<uint2> keyPointIndices; std::vector<float> matchDistances;
			//begin determine if enough overlap
			std::vector<mat4f> estimatedTransforms(numFiltMatches.size());
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(estimatedTransforms.data(), siftManager->getFiltTransformsDEBUG(), sizeof(mat4f)*estimatedTransforms.size(), cudaMemcpyDeviceToHost));
			const std::vector<CUDACachedFrame>& cachedFrames = m_globalCache->getCacheFrames();
			DepthImage32 depthImageCur(m_globalCache->getWidth(), m_globalCache->getHeight()); ColorImageR8G8B8 dummy;
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(depthImageCur.getData(), cachedFrames[curFrame].d_depthDownsampled, sizeof(float)*depthImageCur.getNumPixels(), cudaMemcpyDeviceToHost));
			//end determine if enough overlap
			//evaluate
			std::unordered_set<vec2ui> notEnoughOverlapSet;
			for (unsigned int f = startFrame; f < numFiltMatches.size(); f++) {
				if (f == curFrame) continue;
				if (numFiltMatches[f] > 0) { //found a frame-frame match
					if (notEnoughOverlapSet.find(vec2ui(f, curFrame)) != notEnoughOverlapSet.end()) continue;
					siftManager->getFiltKeyPointIndicesAndMatchDistancesDEBUG(f, keyPointIndices, matchDistances);
					//begin determine if enough overlap
					if (!FrameProjection::isApproxOverlapping(m_globalCache->getIntrinsics(), bbox2f(vec2f::origin, vec2f((float)m_globalCache->getWidth(), (float)m_globalCache->getHeight())),
						estimatedTransforms[f], mat4f::identity())) {
						notEnoughOverlapSet.insert(vec2ui(f, curFrame));
						continue;
					}
					DepthImage32 depthImageOther(m_globalCache->getWidth(), m_globalCache->getHeight()); ColorImageR8G8B8 dummy;
					MLIB_CUDA_SAFE_CALL(cudaMemcpy(depthImageOther.getData(), cachedFrames[f].d_depthDownsampled, sizeof(float)*depthImageOther.getNumPixels(), cudaMemcpyDeviceToHost));
					vec2ui overlap = FrameProjection::computeOverlapBiDirectional(depthImageOther, dummy, depthImageCur, dummy, estimatedTransforms[f],
						m_globalCache->getIntrinsics(), 0.5f, 4.5f, 0.15f, 0.9f, 0.1f, minOverlapPercent, false);
					float overlapPercent = (float)overlap.x / (float)overlap.y;
					if (overlapPercent < minOverlapPercent){
						notEnoughOverlapSet.insert(vec2ui(f, curFrame));
						continue;
					}
					//end determine if enough overlap
					bool flip = false;
					vec2ui frameIndices(f, curFrame);
					if (f > curFrame) {
						frameIndices = vec2ui(curFrame, f);
						flip = true;
					}
					bool good = false; mat4f transform; //todo make sure the ones not in gt set are actually bad
					//if (isLocal) {
					//	frameIndices += m_global->getNumImages() * m_submapSize;
					//	auto it = _gtFrameFrameTransformsLocal.find(frameIndices);
					//	if (it != _gtFrameFrameTransformsLocal.end()) {
					//		good = true;
					//		transform = it->second;
					//	}
					//}
					//else {
					frameIndices *= m_submapSize;
					auto it = _gtFrameFrameTransformsGlobal.find(frameIndices);
					if (it != _gtFrameFrameTransformsGlobal.end()) {
						good = true;
						transform = it->second;
					}
					//}
					if (good) { //eval corrs 
						float meanErr = 0.0f;
						for (unsigned int k = 0; k < keyPointIndices.size(); k++) {
							const SIFTKeyPoint& ki = keyPoints[keyPointIndices[k].x]; const SIFTKeyPoint& kj = keyPoints[keyPointIndices[k].y];
							vec3f pos_i = MatrixConversion::toMlib(siftIntrinsicsInv) * (ki.depth * vec3f(ki.pos.x, ki.pos.y, 1.0f));
							vec3f pos_j = MatrixConversion::toMlib(siftIntrinsicsInv) * (kj.depth * vec3f(kj.pos.x, kj.pos.y, 1.0f));
							if (flip)	meanErr += (transform * pos_j - pos_i).lengthSq();
							else		meanErr += (transform * pos_i - pos_j).lengthSq();
						}
						meanErr /= (float)keyPointIndices.size();
						if (meanErr > 0.04f) good = false;//0.2f^2
						//if (isLocal) {
						//	if (good) _siftMatch_frameFrameLocal.x++;
						//	_siftMatch_frameFrameLocal.y++;
						//}
						//else {
						if (good) _siftMatch_frameFrameGlobal.x++;
						_siftMatch_frameFrameGlobal.y++;
						//}
					}
					else {
						//if (isLocal) { _siftMatch_frameFrameLocal.y++; }
						//else { 
						_siftMatch_frameFrameGlobal.y++;//	}
					}
				}//sift matches found
			}
		}
#endif

		// --- surface area filter
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.start(); }
		//const std::vector<CUDACachedFrame>& cachedFrames = cudaCache->getCacheFrames();
		//SIFTMatchFilter::filterBySurfaceArea(siftManager, cachedFrames);
		siftManager->FilterMatchesBySurfaceAreaCU(curFrame, startFrame, numFrames, siftIntrinsicsInv, GlobalBundlingState::get().s_surfAreaPcaThresh);
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.stop(); TimingLog::getFrameTiming(isLocal).timeMatchFilterSurfaceArea = timer.getElapsedTimeMS(); }

#ifdef DEBUG_PRINT_MATCHING
		std::vector<unsigned int> _numFiltMatchesSA;
		if (printDebug) {
			siftManager->getNumFiltMatchesDEBUG(_numFiltMatchesSA);
			SiftVisualization::printCurrentMatches("debug/matchesSAFilt" + suffix, siftManager, cudaCache, true);

			std::vector<mat4f> filtRelativeTransforms(curFrame);
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(filtRelativeTransforms.data(), siftManager->getFiltTransformsDEBUG(), sizeof(float4x4)*curFrame, cudaMemcpyDeviceToHost));
			for (unsigned int p = 0; p < curFrame; p++) {
				//for (unsigned int p = 12; p < 16; p++) {
				//std::cout << "images (" << p << ", " << curFrame << "):" << std::endl;
				//std::cout << filtRelativeTransforms[p] << std::endl;
				if (_numFiltMatchesSA[p] > 0) saveImPairToPointCloud("debug/", cudaCache, NULL, vec2ui(p, curFrame), filtRelativeTransforms[p]);
			}
			//unsigned int prv = 34;
			//SIFTMatchFilter::visualizeProjError(siftManager, vec2ui(prv, curFrame), cudaCache->getCacheFrames(),
			//	MatrixConversion::toCUDA(cudaCache->getIntrinsics()), MatrixConversion::toCUDA(filtRelativeTransforms[prv].getInverse()),
			//	GlobalAppState::get().s_sensorDepthMin, GlobalAppState::get().s_sensorDepthMax);
			////		//0.1f, 3.0f);
		}
#endif
		// --- dense verify filter
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.start(); }
		//SIFTMatchFilter::filterByDenseVerify(siftManager, cachedFrames);
		const CUDACachedFrame* cachedFramesCUDA = cudaCache->getCacheFramesGPU();
		siftManager->FilterMatchesByDenseVerifyCU(curFrame, startFrame, numFrames, cudaCache->getWidth(), cudaCache->getHeight(), MatrixConversion::toCUDA(cudaCache->getIntrinsics()),
			cachedFramesCUDA, GlobalBundlingState::get().s_projCorrDistThres, GlobalBundlingState::get().s_projCorrNormalThres,
			GlobalBundlingState::get().s_projCorrColorThresh, GlobalBundlingState::get().s_verifySiftErrThresh, GlobalBundlingState::get().s_verifySiftCorrThresh,
			GlobalAppState::get().s_sensorDepthMin, GlobalAppState::get().s_sensorDepthMax); //TODO PARAMS
		//0.1f, 3.0f);
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.stop(); TimingLog::getFrameTiming(isLocal).timeMatchFilterDenseVerify = timer.getElapsedTimeMS(); }

#ifdef DEBUG_PRINT_MATCHING
		std::vector<unsigned int> _numFiltMatchesDV;
		if (printDebug) {
			siftManager->getNumFiltMatchesDEBUG(_numFiltMatchesDV);
			SiftVisualization::printCurrentMatches("debug/filtMatches" + suffix, siftManager, cudaCache, true);
		}
#endif
#ifdef EVALUATE_SPARSE_CORRESPONDENCES
		if (!isLocal) {
			std::vector<unsigned int> numFiltMatches; siftManager->getNumFiltMatchesDEBUG(numFiltMatches);
			MLIB_ASSERT(!numFiltMatches.empty());
			std::vector<uint2> keyPointIndices; std::vector<float> matchDistances;
			//begin determine if enough overlap
			std::vector<mat4f> estimatedTransforms(numFiltMatches.size());
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(estimatedTransforms.data(), siftManager->getFiltTransformsDEBUG(), sizeof(mat4f)*estimatedTransforms.size(), cudaMemcpyDeviceToHost));
			const std::vector<CUDACachedFrame>& cachedFrames = m_globalCache->getCacheFrames();
			DepthImage32 depthImageCur(m_globalCache->getWidth(), m_globalCache->getHeight()); ColorImageR8G8B8 dummy;
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(depthImageCur.getData(), cachedFrames[curFrame].d_depthDownsampled, sizeof(float)*depthImageCur.getNumPixels(), cudaMemcpyDeviceToHost));
			//end determine if enough overlap
			//evaluate
			std::unordered_set<vec2ui> notEnoughOverlapSet;
			for (unsigned int f = startFrame; f < numFiltMatches.size(); f++) {
				if (f == curFrame) continue;
				if (numFiltMatches[f] > 0) { //found a frame-frame match
					if (notEnoughOverlapSet.find(vec2ui(f, curFrame)) != notEnoughOverlapSet.end()) continue;
					siftManager->getFiltKeyPointIndicesAndMatchDistancesDEBUG(f, keyPointIndices, matchDistances);
					//begin determine if enough overlap
					if (!FrameProjection::isApproxOverlapping(m_globalCache->getIntrinsics(), bbox2f(vec2f::origin, vec2f((float)m_globalCache->getWidth(), (float)m_globalCache->getHeight())),
						estimatedTransforms[f], mat4f::identity())) {
						notEnoughOverlapSet.insert(vec2ui(f, curFrame));
						continue;
					}
					DepthImage32 depthImageOther(m_globalCache->getWidth(), m_globalCache->getHeight()); ColorImageR8G8B8 dummy;
					MLIB_CUDA_SAFE_CALL(cudaMemcpy(depthImageOther.getData(), cachedFrames[f].d_depthDownsampled, sizeof(float)*depthImageOther.getNumPixels(), cudaMemcpyDeviceToHost));
					vec2ui overlap = FrameProjection::computeOverlapBiDirectional(depthImageOther, dummy, depthImageCur, dummy, estimatedTransforms[f],
						m_globalCache->getIntrinsics(), 0.5f, 4.5f, 0.15f, 0.9f, 0.1f, minOverlapPercent, false);
					float overlapPercent = (float)overlap.x / (float)overlap.y;
					if (overlapPercent < minOverlapPercent) {
						notEnoughOverlapSet.insert(vec2ui(f, curFrame));
						continue;
					}
					//end determine if enough overlap
					bool flip = false;
					vec2ui frameIndices(f, curFrame);
					if (f > curFrame) {
						frameIndices = vec2ui(curFrame, f);
						flip = true;
					}
					bool good = false; mat4f transform; //todo make sure the ones not in gt set are actually bad
					//if (isLocal) {
					//	frameIndices += m_global->getNumImages() * m_submapSize;
					//	auto it = _gtFrameFrameTransformsLocal.find(frameIndices);
					//	if (it != _gtFrameFrameTransformsLocal.end()) {
					//		good = true;
					//		transform = it->second;
					//	}
					//}
					//else {
					frameIndices *= m_submapSize;
					auto it = _gtFrameFrameTransformsGlobal.find(frameIndices);
					if (it != _gtFrameFrameTransformsGlobal.end()) {
						good = true;
						transform = it->second;
					}
					//}
					if (good) { //eval corrs 
						float meanErr = 0.0f;
						for (unsigned int k = 0; k < keyPointIndices.size(); k++) {
							const SIFTKeyPoint& ki = keyPoints[keyPointIndices[k].x]; const SIFTKeyPoint& kj = keyPoints[keyPointIndices[k].y];
							vec3f pos_i = MatrixConversion::toMlib(siftIntrinsicsInv) * (ki.depth * vec3f(ki.pos.x, ki.pos.y, 1.0f));
							vec3f pos_j = MatrixConversion::toMlib(siftIntrinsicsInv) * (kj.depth * vec3f(kj.pos.x, kj.pos.y, 1.0f));
							if (flip)	meanErr += (transform * pos_j - pos_i).lengthSq();
							else		meanErr += (transform * pos_i - pos_j).lengthSq();
						}
						meanErr /= (float)keyPointIndices.size();
						if (meanErr > 0.04f) good = false;//0.2f^2
						//if (isLocal) {
						//	if (good) _siftVerify_frameFrameLocal.x++;
						//	_siftVerify_frameFrameLocal.y++;
						//}
						//else {
						if (good) _siftVerify_frameFrameGlobal.x++;
						_siftVerify_frameFrameGlobal.y++;
						//}
					}
					else {
						//if (isLocal) { _siftVerify_frameFrameLocal.y++; }
						//else { 
						_siftVerify_frameFrameGlobal.y++; //}
					}
				}//sift matches found
			}
		}
#endif

		// --- filter frames
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.start(); }
		lastMatchedFrame = siftManager->filterFrames(curFrame, startFrame, numFrames);
		// --- add to global correspondences
		MLIB_ASSERT((siftManager->getValidImages()[curFrame] != 0 && lastMatchedFrame != (unsigned int)-1) || (lastMatchedFrame == (unsigned int)-1 && siftManager->getValidImages()[curFrame] == 0)); //TODO REMOVE
		if (lastMatchedFrame != (unsigned int)-1)//if (siftManager->getValidImages()[curFrame] != 0)
			siftManager->AddCurrToResidualsCU(curFrame, startFrame, numFrames, siftIntrinsicsInv);
		//else lastValid = false;
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.stop(); TimingLog::getFrameTiming(isLocal).timeMisc = timer.getElapsedTimeMS(); }
	}

	//return lastValid;
	return lastMatchedFrame;
}

bool SubmapManager::isCurrentLocalValidChunk()
{
	// whether has >1 valid frame
	const std::vector<int>& valid = m_currentLocal->getValidImages();
	const unsigned int numImages = m_currentLocal->getNumImages();
	unsigned int count = 0;
	for (unsigned int i = 0; i < numImages; i++) {
		if (valid[i] != 0) {
			count++;
			if (count > 1) return true;
		}
	}
	return false;
}

unsigned int SubmapManager::getNumNextLocalFrames()
{
	mutex_nextLocal.lock();
	unsigned int numFrames = std::min(m_submapSize, m_nextLocal->getNumImages());
	mutex_nextLocal.unlock();
	return numFrames;
}

void SubmapManager::getCacheIntrinsics(float4x4& intrinsics, float4x4& intrinsicsInv)
{
	intrinsics = MatrixConversion::toCUDA(m_currentLocalCache->getIntrinsics());
	intrinsicsInv = MatrixConversion::toCUDA(m_currentLocalCache->getIntrinsicsInv());
}

void SubmapManager::copyToGlobalCache()
{
	m_globalCache->copyCacheFrameFrom(m_nextLocalCache, 0);
}

bool SubmapManager::optimizeLocal(unsigned int curLocalIdx, unsigned int numNonLinIterations, unsigned int numLinIterations)
{
	bool ret = false;

	mutex_nextLocal.lock();
	SIFTImageManager* siftManager = m_nextLocal;
	CUDACache* cudaCache = m_nextLocalCache;

	const bool buildJt = true;
	const bool removeMaxResidual = false;

	MLIB_ASSERT(m_nextLocal->getNumImages() > 1);
	bool useVerify = GlobalBundlingState::get().s_useLocalVerify;
	m_SparseBundler.align(siftManager, cudaCache, getLocalTrajectoryGPU(curLocalIdx), numNonLinIterations, numLinIterations,
		useVerify, true, false, buildJt, removeMaxResidual, false);
	// still need this for global key fuse

	//if (curLocalIdx >= 70) { //debug vis
	//	saveOptToPointCloud("debug/local-" + std::to_string(curLocalIdx) + ".ply", m_nextLocalCache, m_nextLocal->getValidImages(), getLocalTrajectoryGPU(curLocalIdx), m_nextLocal->getNumImages());
	//}

	// verify
	if (m_SparseBundler.useVerification()) {
		Timer timer;
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.start(); }
		const CUDACachedFrame* cachedFramesCUDA = cudaCache->getCacheFramesGPU();
		int valid = siftManager->VerifyTrajectoryCU(siftManager->getNumImages(), getLocalTrajectoryGPU(curLocalIdx),
			cudaCache->getWidth(), cudaCache->getHeight(), MatrixConversion::toCUDA(cudaCache->getIntrinsics()),
			cachedFramesCUDA, GlobalBundlingState::get().s_projCorrDistThres, GlobalBundlingState::get().s_projCorrNormalThres,
			GlobalBundlingState::get().s_projCorrColorThresh, GlobalBundlingState::get().s_verifyOptErrThresh, GlobalBundlingState::get().s_verifyOptCorrThresh,
			//GlobalAppState::get().s_sensorDepthMin, GlobalAppState::get().s_sensorDepthMax); //TODO PARAMS
			0.1f, 3.0f);
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.stop(); TimingLog::getFrameTiming(true).timeSolve += timer.getElapsedTimeMS(); }

		if (valid == 0) {
			////!!!DEBUGGING
			//vec2ui imageIndices(0, 9);
			//float4x4 transformCur; MLIB_CUDA_SAFE_CALL(cudaMemcpy(&transformCur, getLocalTrajectoryGPU(curLocalIdx) + imageIndices.y, sizeof(float4x4), cudaMemcpyDeviceToHost));
			//float4x4 transformPrv; MLIB_CUDA_SAFE_CALL(cudaMemcpy(&transformPrv, getLocalTrajectoryGPU(curLocalIdx) + imageIndices.x, sizeof(float4x4), cudaMemcpyDeviceToHost));
			//float4x4 transformCurToPrv = transformPrv.getInverse() * transformCur;
			//saveImPairToPointCloud("debug/", cudaCache, getLocalTrajectoryGPU(curLocalIdx), imageIndices);
			//SIFTMatchFilter::visualizeProjError(siftManager, imageIndices, cudaCache->getCacheFrames(),
			//	MatrixConversion::toCUDA(cudaCache->getIntrinsics()), transformCurToPrv, 0.1f, 3.0f);
			//std::cout << "waiting..." << std::endl;
			//getchar();

			//_idxsLocalInvalidVerify.push_back(curLocalIdx);
			//saveOptToPointCloud("debug/opt-" + std::to_string(curLocalIdx) + ".ply", m_nextLocalCache, m_nextLocal->getValidImages(), getLocalTrajectoryGPU(curLocalIdx), m_nextLocal->getNumImages());
			//std::cout << "SAVED " << curLocalIdx << std::endl;
			////!!!DEBUGGING

			if (GlobalBundlingState::get().s_verbose) std::cout << "WARNING: invalid local submap from verify " << curLocalIdx << std::endl;
			//getchar();
			ret = false;
		}
		else
			ret = true;
	}
	else
		ret = true;

	if (ret)
		copyToGlobalCache(); // global cache

	mutex_nextLocal.unlock();
	return ret;
}

#define USE_RETRY

int SubmapManager::computeAndMatchGlobalKeys(unsigned int lastLocalSolved, const float4x4& siftIntrinsics, const float4x4& siftIntrinsicsInv)
{
	int ret = 0; ////!!!TODO FIX
	if ((int)m_global->getNumImages() <= lastLocalSolved) {
		mutex_nextLocal.lock();
		SIFTImageManager* local = m_nextLocal;

		// fuse to global
		Timer timer;
		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.start(); }
		//SIFTImageGPU& curGlobalImage = m_global->createSIFTImageGPU();
		//unsigned int numGlobalKeys = local->FuseToGlobalKeyCU(curGlobalImage, getLocalTrajectoryGPU(lastLocalSolved),
		//	siftIntrinsics, siftIntrinsicsInv);
		//m_global->finalizeSIFTImageGPU(numGlobalKeys);
		local->fuseToGlobal(m_global, siftIntrinsics, getLocalTrajectoryGPU(lastLocalSolved), siftIntrinsicsInv); //TODO GPU version of this

		//fuse local depth frames for global cache
		//m_nextLocalCache->fuseDepthFrames(m_globalCache, local->getValidImagesGPU(), getLocalTrajectoryGPU(lastLocalSolved)); //valid images have been updated in the solve

		if (GlobalBundlingState::get().s_enableGlobalTimings) { cudaDeviceSynchronize(); timer.stop(); TimingLog::getFrameTiming(false).timeSiftDetection = timer.getElapsedTimeMS(); }

		const unsigned int curGlobalFrame = m_global->getCurrentFrame();
		const std::vector<int>& validImagesLocal = local->getValidImages();
		for (unsigned int i = 0; i < std::min(m_submapSize, local->getNumImages()); i++) {
			if (validImagesLocal[i] == 0)
				invalidateImages(curGlobalFrame * m_submapSize + i);
		}
		m_localTrajectoriesValid[curGlobalFrame] = validImagesLocal; m_localTrajectoriesValid[curGlobalFrame].resize(std::min(m_submapSize, local->getNumImages()));
		initializeNextGlobalTransform(curGlobalFrame, (unsigned int)-1); //TODO try local idx too?
		// done with local data!
		finishLocalOpt();
		mutex_nextLocal.unlock();

		//debug vis
		//unsigned int gframe = m_global->getCurrentFrame(); SiftVisualization::printKey("debug/keys/" + std::to_string(gframe) + ".png", m_globalCache, m_global, gframe);

		// match with every other global
		unsigned int lastMatchedGlobal = (unsigned int)-1;
		if (curGlobalFrame > 0) {
			//!!!DEBUGGING
			//if (curGlobalFrame == 200) {
			//	setPrintMatchesDEBUG(true);
			//}
			//!!!DEBUGGING
			lastMatchedGlobal = matchAndFilter(false, m_global, m_globalCache, siftIntrinsicsInv);
			//!!!DEBUGGING
			//if (curGlobalFrame == 200) {
			//	setPrintMatchesDEBUG(false);
			//	std::cout << "waiting..." << std::endl;
			//	getchar();
			//}
			//!!!DEBUGGING
			if (lastMatchedGlobal != (unsigned int)-1 && lastMatchedGlobal + 1 != curGlobalFrame) { //re-initialize to better location based off of last match
				MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_globalTrajectory + curGlobalFrame, d_globalTrajectory + lastMatchedGlobal, sizeof(float4x4), cudaMemcpyDeviceToDevice));
				MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_globalTrajectory + curGlobalFrame + 1, d_globalTrajectory + lastMatchedGlobal, sizeof(float4x4), cudaMemcpyDeviceToDevice));
			}
			if (m_global->getValidImages()[curGlobalFrame]) {
				ret = 1; // ready to solve global
#ifdef USE_RETRY
				tryRevalidation(curGlobalFrame, siftIntrinsicsInv);
#endif
			}
			else {
				if (GlobalBundlingState::get().s_verbose) std::cout << "WARNING: last image (" << curGlobalFrame << ") not valid! no new global images for solve" << std::endl;
				//getchar();
#ifdef USE_RETRY
				m_global->addToRetryList(curGlobalFrame);
#endif
				ret = 2;
			}
		}
		else {
			ret = 0;
			//don't need to initialize global transform since first global frame is identity
		}
	}

	return ret;
}

void SubmapManager::addInvalidGlobalKey()
{
	SIFTImageGPU& curGlobalImage = m_global->createSIFTImageGPU();
	m_global->finalizeSIFTImageGPU(0);
	mutex_nextLocal.lock();
	finishLocalOpt();
	mutex_nextLocal.unlock();
	initializeNextGlobalTransform((unsigned int)-1, (unsigned int)-1);
}

bool SubmapManager::optimizeGlobal(unsigned int numFrames, unsigned int numNonLinIterations, unsigned int numLinIterations, bool isStart, bool removeMaxResidual, bool isScanDone)
{
	bool ret = false;
	const unsigned int numGlobalFrames = m_global->getNumImages();
	const unsigned int curGlobalFrame = m_global->getCurrentFrame();

	const bool useVerify = false;
	bool removed = m_SparseBundler.align(m_global, m_globalCache, d_globalTrajectory, numNonLinIterations, numLinIterations,
		useVerify, false, GlobalBundlingState::get().s_recordSolverConvergence, isStart, removeMaxResidual, isScanDone, m_revalidatedIdx);

	const std::vector<int>& validImagesGlobal = m_global->getValidImages();
	if (removed) {
		// may invalidate already invalidated images
		for (unsigned int i = 0; i < numGlobalFrames; i++) {
			if (validImagesGlobal[i] == 0) {
				invalidateImages(i * m_submapSize, std::min((i + 1)*m_submapSize, numFrames));
			}
		}
	}
	if (removeMaxResidual && validImagesGlobal[numGlobalFrames - 1] != 0) ret = true;
#ifdef EVALUATE_SPARSE_CORRESPONDENCES
	if (isScanDone) {
		static unsigned int counter = 0;
		if (counter >= 150 && (counter % 10) == 0) {
			vec2ui optFrameGlobal = vec2ui(0, 0);
			std::vector<EntryJ> corrs(m_global->getNumGlobalCorrespondences());
			MLIB_ASSERT(!corrs.empty());
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(corrs.data(), m_global->getGlobalCorrespondencesGPU(), sizeof(EntryJ)*corrs.size(), cudaMemcpyDeviceToHost));
			//begin determine if enough overlap
			std::vector<mat4f> estimatedTransforms(m_global->getNumImages());
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(estimatedTransforms.data(), d_globalTrajectory, sizeof(mat4f)*estimatedTransforms.size(), cudaMemcpyDeviceToHost));
			const std::vector<CUDACachedFrame>& cachedFrames = m_globalCache->getCacheFrames();
			//end determine if enough overlap
			//evaluate
			std::unordered_map<vec2ui, vec2f> corrEval; std::unordered_set<vec2ui> notEnoughOverlapSet;
			for (unsigned int f = 0; f < corrs.size(); f++) {
				const EntryJ& c = corrs[f];
				if (c.isValid()) { //found a frame-frame match
					if (notEnoughOverlapSet.find(vec2ui(c.imgIdx_i, c.imgIdx_j)) != notEnoughOverlapSet.end()) continue;
					//begin determine if enough overlap
					if (!FrameProjection::isApproxOverlapping(m_globalCache->getIntrinsics(), bbox2f(vec2f::origin, vec2f((float)m_globalCache->getWidth(), (float)m_globalCache->getHeight())),
						estimatedTransforms[c.imgIdx_i], estimatedTransforms[c.imgIdx_j])) {
						notEnoughOverlapSet.insert(vec2ui(c.imgIdx_i, c.imgIdx_j));
						continue;
					}
					DepthImage32 depthImage0(m_globalCache->getWidth(), m_globalCache->getHeight()); ColorImageR8G8B8 dummy;
					DepthImage32 depthImage1(m_globalCache->getWidth(), m_globalCache->getHeight());
					MLIB_CUDA_SAFE_CALL(cudaMemcpy(depthImage0.getData(), cachedFrames[c.imgIdx_i].d_depthDownsampled, sizeof(float)*depthImage0.getNumPixels(), cudaMemcpyDeviceToHost));
					MLIB_CUDA_SAFE_CALL(cudaMemcpy(depthImage1.getData(), cachedFrames[c.imgIdx_j].d_depthDownsampled, sizeof(float)*depthImage1.getNumPixels(), cudaMemcpyDeviceToHost));
					vec2ui overlap = FrameProjection::computeOverlapBiDirectional(depthImage0, dummy, depthImage1, dummy, estimatedTransforms[c.imgIdx_j].getInverse() * estimatedTransforms[c.imgIdx_i],
						m_globalCache->getIntrinsics(), 0.5f, 4.5f, 0.15f, 0.9f, 0.1f, 0.3f, false);
					float overlapPercent = (float)overlap.x / (float)overlap.y;
					if (overlapPercent < 0.3f) {
						notEnoughOverlapSet.insert(vec2ui(c.imgIdx_i, c.imgIdx_j));
						continue;
					}
					//end determine if enough overlap
					bool flip = false;
					vec2ui frameIndices(c.imgIdx_i, c.imgIdx_j);
					if (c.imgIdx_j < c.imgIdx_i) {
						frameIndices = vec2ui(c.imgIdx_j, c.imgIdx_i);
						flip = true;
					}
					frameIndices *= m_submapSize;
					auto it = _gtFrameFrameTransformsGlobal.find(frameIndices);
					if (it != _gtFrameFrameTransformsGlobal.end()) {
						float err2 = flip ?
							(it->second * vec3f(c.pos_j.x, c.pos_j.y, c.pos_j.z) - vec3f(c.pos_i.x, c.pos_i.y, c.pos_i.z)).lengthSq() :
							(it->second * vec3f(c.pos_i.x, c.pos_i.y, c.pos_i.z) - vec3f(c.pos_j.x, c.pos_j.y, c.pos_j.z)).lengthSq();
						auto itCorrEval = corrEval.find(frameIndices);
						if (itCorrEval == corrEval.end()) corrEval[frameIndices] = vec2f(err2, 1.0f);
						else itCorrEval->second += vec2f(err2, 1.0f);
					}
					else {
						corrEval[frameIndices] = vec2f(-std::numeric_limits<float>::infinity());
						////debugging
						//vec2ui overlap = FrameProjection::computeOverlapBiDirectional(depthImage0, dummy, depthImage1, dummy, estimatedTransforms[c.imgIdx_j].getInverse() * estimatedTransforms[c.imgIdx_i],
						//	m_globalCache->getIntrinsics(), 0.5f, 4.5f, 0.15f, 0.9f, 0.1f, 0.3f, true);
						std::cout << "FOUND BAD MATCH BETWEEN FRAMES " << frameIndices << ", overlap = " << overlap.x << " / " << overlap.y << " = " << overlapPercent << std::endl;
						//int a = 5;
						////debugging
					}
				} //valid corrs
			}//correspondences
			for (const auto& ce : corrEval) {
				if (ce.second.x != -std::numeric_limits<float>::infinity()) {
					float meanErr = ce.second.x / ce.second.y;
					if (meanErr <= 0.04f) optFrameGlobal.x++;
					optFrameGlobal.y++;
				}
				else {
					optFrameGlobal.y++;
				}
			}
			_opt_frameFrameGlobal = optFrameGlobal;
		}
		counter++;
	}
#endif

	////debugging
	//if (isScanDone) {
	//	static unsigned int counter = 0;
	//	if (counter == 20) {
	//		m_global->saveToFile("debug/global.sift");
	//		m_globalCache->saveToFile("debug/global.cache");
	//		//std::vector<mat4f> keysTrajectory(m_global->getNumImages());
	//		//MLIB_CUDA_SAFE_CALL(cudaMemcpy(keysTrajectory.data(), d_globalTrajectory, sizeof(mat4f)*keysTrajectory.size(), cudaMemcpyDeviceToHost));
	//		{
	//			std::vector<mat4f> completeTrajectory(2991); //debugging
	//			MLIB_CUDA_SAFE_CALL(cudaMemcpy(completeTrajectory.data(), d_completeTrajectory, sizeof(mat4f)*completeTrajectory.size(), cudaMemcpyDeviceToHost));
	//			BinaryDataStreamFile s("debug/complete.trajectory", true);
	//			s << completeTrajectory;
	//			s.closeStream();
	//		}
	//		{
	//			std::vector<mat4f> localTrajectories(300*11); //debugging
	//			MLIB_CUDA_SAFE_CALL(cudaMemcpy(localTrajectories.data(), d_localTrajectories, sizeof(mat4f)*localTrajectories.size(), cudaMemcpyDeviceToHost));
	//			BinaryDataStreamFile s("debug/locals.trajectory", true);
	//			s << localTrajectories;
	//			s.closeStream();
	//		}
	//		std::cout << "waiting..." << std::endl;
	//		getchar();
	//	}
	//	counter++;
	//}
	////debugging
	return ret;
}

void SubmapManager::saveCompleteTrajectory(const std::string& filename, unsigned int numTransforms) const
{
	std::vector<mat4f> completeTrajectory(numTransforms);
	MLIB_CUDA_SAFE_CALL(cudaMemcpy(completeTrajectory.data(), d_completeTrajectory, sizeof(mat4f)*completeTrajectory.size(), cudaMemcpyDeviceToHost));

	BinaryDataStreamFile s(filename, true);
	s << completeTrajectory;
	s.closeStream();
}

void SubmapManager::saveSiftTrajectory(const std::string& filename, unsigned int numTransforms) const
{
	std::vector<mat4f> siftTrjectory(numTransforms);
	MLIB_CUDA_SAFE_CALL(cudaMemcpy(siftTrjectory.data(), d_siftTrajectory, sizeof(mat4f)*siftTrjectory.size(), cudaMemcpyDeviceToHost));

	BinaryDataStreamFile s(filename, true);
	s << siftTrjectory;
	s.closeStream();
}

void SubmapManager::saveOptToPointCloud(const std::string& filename, const CUDACache* cudaCache, const std::vector<int>& valid,
	const float4x4* d_transforms, unsigned int numFrames, bool saveFrameByFrame /*= false*/)
{
	// local transforms: d_transforms = getLocalTrajectoryGPU(localIdx);
	// local cudaCache: cudaCache = m_nextLocalCache
	// global transforms: d_transforms = d_globalTrajectory;
	// global cudaCache: cudaCache = m_globalCache

	//transforms
	std::vector<mat4f> transforms(numFrames);
	MLIB_CUDA_SAFE_CALL(cudaMemcpy(transforms.data(), d_transforms, sizeof(float4x4)*numFrames, cudaMemcpyDeviceToHost));
	//frames
	ColorImageR32G32B32A32 camPosition;
	ColorImageR32 intensity;
	camPosition.allocate(cudaCache->getWidth(), cudaCache->getHeight());
	intensity.allocate(cudaCache->getWidth(), cudaCache->getHeight());
	const std::vector<CUDACachedFrame>& cacheFrames = cudaCache->getCacheFrames();

	const std::string outFrameByFrame = "debug/frames/";
	if (saveFrameByFrame && !util::directoryExists(outFrameByFrame)) util::makeDirectory(outFrameByFrame);

	PointCloudf pc;
	for (unsigned int f = 0; f < numFrames; f++) {
		if (valid[f] == 0) continue;

		MLIB_CUDA_SAFE_CALL(cudaMemcpy(camPosition.getData(), cacheFrames[f].d_cameraposDownsampled, sizeof(float4)*camPosition.getNumPixels(), cudaMemcpyDeviceToHost));
		MLIB_CUDA_SAFE_CALL(cudaMemcpy(intensity.getData(), cacheFrames[f].d_intensityDownsampled, sizeof(uchar4)*intensity.getNumPixels(), cudaMemcpyDeviceToHost));

		PointCloudf framePc;

		for (unsigned int i = 0; i < camPosition.getNumPixels(); i++) {
			const vec4f& p = camPosition.getData()[i];
			if (p.x != -std::numeric_limits<float>::infinity()) {
				pc.m_points.push_back(transforms[f] * p.getVec3());
				const float c = intensity.getData()[i];
				pc.m_colors.push_back(vec4f(c));

				if (saveFrameByFrame) {
					framePc.m_points.push_back(pc.m_points.back());
					framePc.m_colors.push_back(pc.m_colors.back());
				}
			}
		}
		if (saveFrameByFrame) {
			PointCloudIOf::saveToFile(outFrameByFrame + std::to_string(f) + ".ply", framePc);
		}
	}
	PointCloudIOf::saveToFile(filename, pc);
}

void SubmapManager::saveImPairToPointCloud(const std::string& prefix, const CUDACache* cudaCache, const float4x4* d_transforms, const vec2ui& imageIndices, const mat4f& transformCurToPrv /*= mat4f::zero()*/) const
{
	// local transforms: d_transforms = getLocalTrajectoryGPU(localIdx);
	// local cudaCache: cudaCache = m_nextLocalCache
	// global transforms: d_transforms = d_globalTrajectory;
	// global cudaCache: cudaCache = m_globalCache

	//transforms
	std::vector<mat4f> transforms(2);
	if (d_transforms) {
		std::vector<mat4f> allTransforms(std::max(imageIndices.x, imageIndices.y) + 1);
		MLIB_CUDA_SAFE_CALL(cudaMemcpy(allTransforms.data(), d_transforms, sizeof(float4x4)*transforms.size(), cudaMemcpyDeviceToHost));
		transforms[0] = allTransforms[imageIndices.x];
		transforms[1] = allTransforms[imageIndices.y];
	}
	else {
		if (transformCurToPrv[0] == 0) {
			std::cout << "no valid transform between " << imageIndices << std::endl;
			return;
		}
		transforms[0] = transformCurToPrv;
		transforms[1] = mat4f::identity();
	}
	//frames
	ColorImageR32G32B32A32 camPosition;
	ColorImageR32 intensity;
	camPosition.allocate(cudaCache->getWidth(), cudaCache->getHeight());
	intensity.allocate(cudaCache->getWidth(), cudaCache->getHeight());
	const std::vector<CUDACachedFrame>& cacheFrames = cudaCache->getCacheFrames();

	bool saveFrameByFrame = true;
	const std::string dir = util::directoryFromPath(prefix);
	if (saveFrameByFrame && !util::directoryExists(dir)) util::makeDirectory(dir);

	PointCloudf pc;
	for (unsigned int i = 0; i < 2; i++) {
		mat4f transform = transforms[i];
		unsigned int f = imageIndices[i];
		MLIB_CUDA_SAFE_CALL(cudaMemcpy(camPosition.getData(), cacheFrames[f].d_cameraposDownsampled, sizeof(float4)*camPosition.getNumPixels(), cudaMemcpyDeviceToHost));
		MLIB_CUDA_SAFE_CALL(cudaMemcpy(intensity.getData(), cacheFrames[f].d_intensityDownsampled, sizeof(float)*intensity.getNumPixels(), cudaMemcpyDeviceToHost));

		PointCloudf framePc;

		for (unsigned int i = 0; i < camPosition.getNumPixels(); i++) {
			const vec4f& p = camPosition.getData()[i];
			if (p.x != -std::numeric_limits<float>::infinity()) {
				pc.m_points.push_back(transform * p.getVec3());
				const float c = intensity.getData()[i];
				pc.m_colors.push_back(vec4f(c));

				if (saveFrameByFrame) {
					framePc.m_points.push_back(pc.m_points.back());
					framePc.m_colors.push_back(pc.m_colors.back());
				}
			}
		}
		if (saveFrameByFrame) {
			PointCloudIOf::saveToFile(dir + std::to_string(f) + ".ply", framePc);
		}
	}
	PointCloudIOf::saveToFile(prefix + "_" + std::to_string(imageIndices.x) + "-" + std::to_string(imageIndices.y) + ".ply", pc);
}

void SubmapManager::saveGlobalSiftManagerAndCache(const std::string& prefix) const
{
	const std::string siftFile = prefix + ".sift";
	const std::string cacheFile = prefix + ".cache";
	m_global->saveToFile(siftFile);
	m_globalCache->saveToFile(cacheFile);
}

void SubmapManager::setEndSolveGlobalDenseWeights()
{
	GlobalBundlingState::get().s_numGlobalNonLinIterations = 3;
	const unsigned int maxNumIts = GlobalBundlingState::get().s_numGlobalNonLinIterations;
	std::vector<float> sparseWeights(maxNumIts, 1.0f);
	std::vector<float> denseDepthWeights(maxNumIts, 15.0f);
	std::vector<float> denseColorWeights(maxNumIts, 0.0f);
	m_SparseBundler.setGlobalWeights(sparseWeights, denseDepthWeights, denseColorWeights, true);
	std::cout << "set end solve global dense weights" << std::endl;
}

void SubmapManager::updateTrajectory(unsigned int curFrame)
{
	MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_imageInvalidateList, m_invalidImagesList.data(), sizeof(int)*curFrame, cudaMemcpyHostToDevice));

	updateTrajectoryCU(d_globalTrajectory, m_global->getNumImages(),
		d_completeTrajectory, curFrame,
		d_localTrajectories, m_submapSize + 1, m_global->getNumImages(),
		d_imageInvalidateList);
}

void SubmapManager::tryRevalidation(unsigned int curGlobalFrame, const float4x4& siftIntrinsicsInv, bool isScanningDone /*= false*/)
{
	m_revalidatedIdx = (unsigned int)-1;
	if (m_continueRetry < 0) return; // nothing to do
	// see if have any invalid images which match
	unsigned int idx;
	if (m_global->getTopRetryImage(idx)) {
		if (isScanningDone) {
			if (m_continueRetry == 0) {
				m_continueRetry = idx;
			}
			else if (m_continueRetry == idx) {
				m_continueRetry = -1;
				return; // nothing more to do (looped around again)
			}
		}
		m_global->setCurrentFrame(idx);

		unsigned int lastMatchedGlobal = matchAndFilter(false, m_global, m_globalCache, siftIntrinsicsInv);

		if (m_global->getValidImages()[idx] != 0) { //validate
			//initialize to better transform
			MLIB_ASSERT(lastMatchedGlobal != (unsigned int)-1);
			MLIB_CUDA_SAFE_CALL(cudaMemcpy(d_globalTrajectory + idx, d_globalTrajectory + lastMatchedGlobal, sizeof(float4x4), cudaMemcpyDeviceToDevice));
			//validate chunk images
			const std::vector<int>& validLocal = m_localTrajectoriesValid[idx];
			for (unsigned int i = 0; i < validLocal.size(); i++) {
				if (validLocal[i] == 1)	validateImages(idx * m_submapSize + i);
			}
			if (GlobalBundlingState::get().s_verbose) std::cout << "re-validating " << idx << std::endl;
			m_revalidatedIdx = idx;
		}
		else {
			m_global->addToRetryList(idx);
		}
		//reset
		m_global->setCurrentFrame(curGlobalFrame);
	}
}

void SubmapManager::invalidateLastGlobalFrame() {
	if (m_global->getNumImages() <= 1) { //can't invalidate first chunk
		std::cout << "INVALID FIRST CHUNK" << std::endl; // for nyu data, check for flipped depth/color frames
		std::ofstream s(util::directoryFromPath(GlobalAppState::get().s_binaryDumpSensorFile) + "processed.txt");
		s << "valid = false" << std::endl;
		s << "INVALID_FIRST_CHUNK" << std::endl;
		s.close();
		ConditionManager::setExit();
	}
	//MLIB_ASSERT(m_global->getNumImages() > 1);
	m_global->invalidateFrame(m_global->getNumImages() - 1);
}

void SubmapManager::saveGlobalSiftCorrespondences(const std::string& filename) const
{
	std::vector<EntryJ> correspondences(m_global->getNumGlobalCorrespondences());
	if (correspondences.empty()) {
		std::cout << "no global sift corrs to save" << std::endl;
		return;
	}
	MLIB_CUDA_SAFE_CALL(cudaMemcpy(correspondences.data(), m_global->getGlobalCorrespondencesGPU(), sizeof(EntryJ)*correspondences.size(), cudaMemcpyDeviceToHost));
	BinaryDataStreamFile s(filename, true);
	s << (UINT64)correspondences.size();
	s.writeData((const BYTE*)correspondences.data(), sizeof(EntryJ)*correspondences.size());
	s.closeStream();
	std::cout << "saved " << correspondences.size() << " corrs" << std::endl;
}
