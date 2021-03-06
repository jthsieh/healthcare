/*******************************************************************************
*                                                                              *
*   PrimeSense NiTE 2.0 - User Viewer Sample                                   *
*   Copyright (C) 2012 PrimeSense Ltd.                                         *
*                                                                              *
*******************************************************************************/

#include "Viewer.h"

#if (ONI_PLATFORM == ONI_PLATFORM_MACOSX)
        #include <GLUT/glut.h>
#else
        #include <GL/glut.h>
#endif

#include "../Common/NiteSampleUtilities.h"
#include <sys/stat.h>

#define GL_WIN_SIZE_X	320
#define GL_WIN_SIZE_Y	240
#define TEXTURE_SIZE	512

#define DEFAULT_DISPLAY_MODE	DISPLAY_MODE_DEPTH

#define MIN_NUM_CHUNKS(data_size, chunk_size)	((((data_size)-1) / (chunk_size) + 1))
#define MIN_CHUNKS_SIZE(data_size, chunk_size)	(MIN_NUM_CHUNKS(data_size, chunk_size) * (chunk_size))

SampleViewer* SampleViewer::ms_self = NULL;

bool g_drawSkeleton = true;
bool g_drawCenterOfMass = false;
bool g_drawStatusLabel = true;
bool g_drawBoundingBox = true;
bool g_drawBackground = true;
bool g_drawDepth = true;
bool g_drawFrameId = false;
bool g_pause = false;
bool g_capture1 = false; // only capture 1 frame
bool g_capture2 = false; // capture continuously
bool g_getBackground = true;

int nFrame = 0;

int *label;

float sideJoints[N_JOINTS][5];
float topJoints[N_JOINTS][5];

int g_nXRes = 0, g_nYRes = 0;
string outDir = "/Users/alan/Documents/research/healthcare/src/poseEstimation/UserViewer/data";

// time to hold in pose to exit program. In milliseconds.
const int g_poseTimeoutToExit = 2000;

void SampleViewer::glutIdle()
{
	glutPostRedisplay();
}

void SampleViewer::glutDisplay()
{
	SampleViewer::ms_self->Display();
}

void SampleViewer::glutKeyboard(unsigned char key, int x, int y)
{
	SampleViewer::ms_self->OnKey(key, x, y);
}

SampleViewer::SampleViewer(const char* strSampleName) : m_poseUser(0)
{
	ms_self = this;
	strncpy(m_strSampleName, strSampleName, ONI_MAX_STR);
	m_pUserTracker = new nite::UserTracker;
    bgSubtractor = new BgSubtractor(320, 240);
}

SampleViewer::~SampleViewer()
{
	Finalize();

	delete[] m_pTexMap;

	ms_self = NULL;
}

void SampleViewer::Finalize()
{
	delete m_pUserTracker;
	nite::NiTE::shutdown();
	openni::OpenNI::shutdown();
    m_deviceSide.close();
    m_deviceTop.close();
}

openni::Status SampleViewer::Init(int argc, char **argv)
{
	m_pTexMap = NULL;

	openni::Status rc = openni::OpenNI::initialize();
	if (rc != openni::STATUS_OK)
	{
		printf("Failed to initialize OpenNI\n%s\n", openni::OpenNI::getExtendedError());
		return rc;
	}

    openni::Array<openni::DeviceInfo> deviceInfoList;
    openni::OpenNI::enumerateDevices(&deviceInfoList);
    
    for (int i = 0; i < deviceInfoList.getSize(); ++i) {
        const openni::DeviceInfo &info = deviceInfoList[i];
        string uri = info.getUri();
    }
    
    /*
	const char* deviceUri = openni::ANY_DEVICE;
	for (int i = 1; i < argc-1; ++i)
	{
		if (strcmp(argv[i], "-device") == 0)
		{
			deviceUri = argv[i+1];
			break;
		}
	}
    */
    
    assert(deviceInfoList.getSize() <= 2);

	rc = m_deviceSide.open(deviceInfoList[0].getUri());
	if (rc != openni::STATUS_OK) {
		printf("Failed to open device\n%s\n", openni::OpenNI::getExtendedError());
		return rc;
	}
    if (deviceInfoList.getSize() == 2) {
        rc = m_deviceTop.open(deviceInfoList[1].getUri());
        if (rc != openni::STATUS_OK) {
            printf("Failed to open device\n%s\n", openni::OpenNI::getExtendedError());
            return rc;
        }
    }

	nite::NiTE::initialize();

	if (m_pUserTracker->create(&m_deviceSide) != nite::STATUS_OK)
	{
		return openni::STATUS_ERROR;
	}
    
    rc = InitOpenGL(argc, argv);
    
    rc = depthStreamTop.create(m_deviceTop, openni::SENSOR_DEPTH);
    rc = depthStreamTop.start();
    
    sideSkel = Mat::zeros(GL_WIN_SIZE_Y, GL_WIN_SIZE_X, CV_8UC3);
    topSkel = Mat::zeros(GL_WIN_SIZE_Y, GL_WIN_SIZE_X, CV_8UC3);
    namedWindow("Side", WINDOW_AUTOSIZE);
    moveWindow("Side", 0, 600);
    namedWindow("Top", WINDOW_AUTOSIZE);
    moveWindow("Top", 0, 0);
    namedWindow("DepthTop", WINDOW_AUTOSIZE);
    moveWindow("DepthTop", 1120, 600);
    namedWindow("Label", WINDOW_AUTOSIZE);
    moveWindow("Label", 300, 200);
    namedWindow("Mask", WINDOW_AUTOSIZE);
    moveWindow("Mask", 1120, 0);
    
	return rc;
}

openni::Status SampleViewer::Run()	//Does not return
{
	glutMainLoop();

	return openni::STATUS_OK;
}

float Colors[][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}};
int colorCount = 3;

#define MAX_USERS 10
bool g_visibleUsers[MAX_USERS] = {false};
nite::SkeletonState g_skeletonStates[MAX_USERS] = {nite::SKELETON_NONE};
char g_userStatusLabels[MAX_USERS][100] = {{0}};

char g_generalMessage[100] = {0};

#define USER_MESSAGE(msg) {\
	sprintf(g_userStatusLabels[user.getId()],"%s", msg);\
	printf("[%08llu] User #%d:\t%s\n",ts, user.getId(),msg);}

void updateUserState(const nite::UserData& user, uint64_t ts)
{
	if (user.isNew())
	{
		USER_MESSAGE("New");
	}
	else if (user.isVisible() && !g_visibleUsers[user.getId()])
		printf("[%08llu] User #%d:\tVisible\n", ts, user.getId());
	else if (!user.isVisible() && g_visibleUsers[user.getId()])
		printf("[%08llu] User #%d:\tOut of Scene\n", ts, user.getId());
	else if (user.isLost())
	{
		USER_MESSAGE("Lost");
	}
	g_visibleUsers[user.getId()] = user.isVisible();


	if(g_skeletonStates[user.getId()] != user.getSkeleton().getState())
	{
		switch(g_skeletonStates[user.getId()] = user.getSkeleton().getState())
		{
		case nite::SKELETON_NONE:
			USER_MESSAGE("Stopped tracking.")
			break;
		case nite::SKELETON_CALIBRATING:
			USER_MESSAGE("Calibrating...")
			break;
		case nite::SKELETON_TRACKED:
			USER_MESSAGE("Tracking!")
			break;
		case nite::SKELETON_CALIBRATION_ERROR_NOT_IN_POSE:
		case nite::SKELETON_CALIBRATION_ERROR_HANDS:
		case nite::SKELETON_CALIBRATION_ERROR_LEGS:
		case nite::SKELETON_CALIBRATION_ERROR_HEAD:
		case nite::SKELETON_CALIBRATION_ERROR_TORSO:
			USER_MESSAGE("Calibration Failed... :-|")
			break;
		}
	}
}

#ifndef USE_GLES
void glPrintString(void *font, const char *str)
{
	int i,l = (int)strlen(str);

	for(i=0; i<l; i++)
	{   
		glutBitmapCharacter(font,*str++);
	}   
}
#endif

void DrawStatusLabel(nite::UserTracker* pUserTracker, const nite::UserData& user)
{
	int color = user.getId() % colorCount;
	glColor3f(1.0f - Colors[color][0], 1.0f - Colors[color][1], 1.0f - Colors[color][2]);

	float x,y;
	pUserTracker->convertJointCoordinatesToDepth(user.getCenterOfMass().x, user.getCenterOfMass().y, user.getCenterOfMass().z, &x, &y);
	x *= GL_WIN_SIZE_X/g_nXRes;
	y *= GL_WIN_SIZE_Y/g_nYRes;
	char *msg = g_userStatusLabels[user.getId()];
	glRasterPos2i(x-((strlen(msg)/2)*8),y);
	glPrintString(GLUT_BITMAP_HELVETICA_18, msg);
}

void DrawFrameId(int frameId)
{
	char buffer[80] = "";
	sprintf(buffer, "%d", frameId);
	glColor3f(1.0f, 0.0f, 0.0f);
	glRasterPos2i(20, 20);
	glPrintString(GLUT_BITMAP_HELVETICA_18, buffer);
}

void DrawCenterOfMass(nite::UserTracker* pUserTracker, const nite::UserData& user)
{
	glColor3f(1.0f, 1.0f, 1.0f);

	float coordinates[3] = {0};

	pUserTracker->convertJointCoordinatesToDepth(user.getCenterOfMass().x, user.getCenterOfMass().y, user.getCenterOfMass().z, &coordinates[0], &coordinates[1]);

	coordinates[0] *= GL_WIN_SIZE_X/g_nXRes;
	coordinates[1] *= GL_WIN_SIZE_Y/g_nYRes;
	glPointSize(8);
	glVertexPointer(3, GL_FLOAT, 0, coordinates);
	glDrawArrays(GL_POINTS, 0, 1);

}

float getSize(const nite::UserData& user) {
    float dx = user.getBoundingBox().max.x - user.getBoundingBox().min.x;
    float dy = user.getBoundingBox().max.y - user.getBoundingBox().min.y;
    
    return dx*dy;
}

void DrawBoundingBox(const nite::UserData& user)
{
	glColor3f(1.0f, 1.0f, 1.0f);

	float coordinates[] =
	{
		user.getBoundingBox().max.x, user.getBoundingBox().max.y, 0,
		user.getBoundingBox().max.x, user.getBoundingBox().min.y, 0,
		user.getBoundingBox().min.x, user.getBoundingBox().min.y, 0,
		user.getBoundingBox().min.x, user.getBoundingBox().max.y, 0,
	};
	coordinates[0]  *= GL_WIN_SIZE_X/g_nXRes;
	coordinates[1]  *= GL_WIN_SIZE_Y/g_nYRes;
	coordinates[3]  *= GL_WIN_SIZE_X/g_nXRes;
	coordinates[4]  *= GL_WIN_SIZE_Y/g_nYRes;
	coordinates[6]  *= GL_WIN_SIZE_X/g_nXRes;
	coordinates[7]  *= GL_WIN_SIZE_Y/g_nYRes;
	coordinates[9]  *= GL_WIN_SIZE_X/g_nXRes;
	coordinates[10] *= GL_WIN_SIZE_Y/g_nYRes;

	glPointSize(2);
	glVertexPointer(3, GL_FLOAT, 0, coordinates);
	glDrawArrays(GL_LINE_LOOP, 0, 4);

}

string getJointName(nite::JointType jointType) {
    string name;
    switch (jointType) {
        case nite::JOINT_HEAD:
            name = "JOINT_HEAD";
            break;
        case nite::JOINT_NECK:
            name = "JOINT_NECK";
            break;
        case nite::JOINT_LEFT_SHOULDER:
            name = "JOINT_LEFT_SHOULDER";
            break;
        case nite::JOINT_RIGHT_SHOULDER:
            name = "JOINT_RIGHT_SHOULDER";
            break;
        case nite::JOINT_LEFT_ELBOW:
            name = "JOINT_LEFT_ELBOW";
            break;
        case nite::JOINT_RIGHT_ELBOW:
            name = "JOINT_RIGHT_ELBOW";
            break;
        case nite::JOINT_LEFT_HAND:
            name = "JOINT_LEFT_HAND";
            break;
        case nite::JOINT_RIGHT_HAND:
            name = "JOINT_RIGHT_HAND";
            break;
        case nite::JOINT_TORSO:
            name = "JOINT_TORSO";
            break;
        case nite::JOINT_LEFT_HIP:
            name = "JOINT_LEFT_HIP";
            break;
        case nite::JOINT_RIGHT_HIP:
            name = "JOINT_RIGHT_HIP";
            break;
        case nite::JOINT_LEFT_KNEE:
            name = "JOINT_LEFT_KNEE";
            break;
        case nite::JOINT_RIGHT_KNEE:
            name = "JOINT_RIGHT_KNEE";
            break;
        case nite::JOINT_LEFT_FOOT:
            name = "JOINT_LEFT_FOOT";
            break;
        case nite::JOINT_RIGHT_FOOT:
            name = "JOINT_RIGHT_FOOT";
            break;
    }
    return name;
}

void SaveJoint(nite::UserTracker* pUserTracker, const nite::UserData& userData, nite::JointType jointType) {
    nite::Point3f joint = userData.getSkeleton().getJoint(jointType).getPosition();
    sideJoints[jointType][0] = joint.x;
    sideJoints[jointType][1] = joint.y;
    sideJoints[jointType][2] = joint.z;
    
    side2top(sideJoints, topJoints);
    
    pUserTracker->convertJointCoordinatesToDepth(joint.x, joint.y, joint.z, &sideJoints[jointType][3], &sideJoints[jointType][4]);
    pUserTracker->convertJointCoordinatesToDepth(topJoints[jointType][0], topJoints[jointType][1], topJoints[jointType][2], &topJoints[jointType][3], &topJoints[jointType][4]);
}

void DrawLimb(nite::UserTracker* pUserTracker, const nite::SkeletonJoint& joint1, const nite::SkeletonJoint& joint2, int color)
{
	float coordinates[6] = {0};
	pUserTracker->convertJointCoordinatesToDepth(joint1.getPosition().x, joint1.getPosition().y, joint1.getPosition().z, &coordinates[0], &coordinates[1]);
	pUserTracker->convertJointCoordinatesToDepth(joint2.getPosition().x, joint2.getPosition().y, joint2.getPosition().z, &coordinates[3], &coordinates[4]);

	coordinates[0] *= GL_WIN_SIZE_X/g_nXRes;
	coordinates[1] *= GL_WIN_SIZE_Y/g_nYRes;
	coordinates[3] *= GL_WIN_SIZE_X/g_nXRes;
	coordinates[4] *= GL_WIN_SIZE_Y/g_nYRes;

	if (joint1.getPositionConfidence() == 1 && joint2.getPositionConfidence() == 1)
	{
		glColor3f(1.0f - Colors[color][0], 1.0f - Colors[color][1], 1.0f - Colors[color][2]);
	}
	else if (joint1.getPositionConfidence() < 0.5f || joint2.getPositionConfidence() < 0.5f)
	{
		return;
	}
	else
	{
		glColor3f(.5, .5, .5);
	}
	glPointSize(2);
	glVertexPointer(3, GL_FLOAT, 0, coordinates);
	glDrawArrays(GL_LINES, 0, 2);

	glPointSize(10);
	if (joint1.getPositionConfidence() == 1)
	{
		glColor3f(1.0f - Colors[color][0], 1.0f - Colors[color][1], 1.0f - Colors[color][2]);
	}
	else
	{
		glColor3f(.5, .5, .5);
	}
	glVertexPointer(3, GL_FLOAT, 0, coordinates);
	glDrawArrays(GL_POINTS, 0, 1);

	if (joint2.getPositionConfidence() == 1)
	{
		glColor3f(1.0f - Colors[color][0], 1.0f - Colors[color][1], 1.0f - Colors[color][2]);
	}
	else
	{
		glColor3f(.5, .5, .5);
	}
	glVertexPointer(3, GL_FLOAT, 0, coordinates+3);
	glDrawArrays(GL_POINTS, 0, 1);
}

void DrawSkeleton(nite::UserTracker* pUserTracker, const nite::UserData& userData)
{
	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_HEAD), userData.getSkeleton().getJoint(nite::JOINT_NECK), userData.getId() % colorCount);

	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_LEFT_SHOULDER), userData.getSkeleton().getJoint(nite::JOINT_LEFT_ELBOW), userData.getId() % colorCount);
	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_LEFT_ELBOW), userData.getSkeleton().getJoint(nite::JOINT_LEFT_HAND), userData.getId() % colorCount);

	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_RIGHT_SHOULDER), userData.getSkeleton().getJoint(nite::JOINT_RIGHT_ELBOW), userData.getId() % colorCount);
	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_RIGHT_ELBOW), userData.getSkeleton().getJoint(nite::JOINT_RIGHT_HAND), userData.getId() % colorCount);

	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_LEFT_SHOULDER), userData.getSkeleton().getJoint(nite::JOINT_RIGHT_SHOULDER), userData.getId() % colorCount);

	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_LEFT_SHOULDER), userData.getSkeleton().getJoint(nite::JOINT_TORSO), userData.getId() % colorCount);
	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_RIGHT_SHOULDER), userData.getSkeleton().getJoint(nite::JOINT_TORSO), userData.getId() % colorCount);

	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_TORSO), userData.getSkeleton().getJoint(nite::JOINT_LEFT_HIP), userData.getId() % colorCount);
	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_TORSO), userData.getSkeleton().getJoint(nite::JOINT_RIGHT_HIP), userData.getId() % colorCount);

	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_LEFT_HIP), userData.getSkeleton().getJoint(nite::JOINT_RIGHT_HIP), userData.getId() % colorCount);

	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_LEFT_HIP), userData.getSkeleton().getJoint(nite::JOINT_LEFT_KNEE), userData.getId() % colorCount);
	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_LEFT_KNEE), userData.getSkeleton().getJoint(nite::JOINT_LEFT_FOOT), userData.getId() % colorCount);

	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_RIGHT_HIP), userData.getSkeleton().getJoint(nite::JOINT_RIGHT_KNEE), userData.getId() % colorCount);
	DrawLimb(pUserTracker, userData.getSkeleton().getJoint(nite::JOINT_RIGHT_KNEE), userData.getSkeleton().getJoint(nite::JOINT_RIGHT_FOOT), userData.getId() % colorCount);
    
    SaveJoint(pUserTracker, userData, nite::JOINT_HEAD);
    SaveJoint(pUserTracker, userData, nite::JOINT_NECK);
    SaveJoint(pUserTracker, userData, nite::JOINT_LEFT_SHOULDER);
    SaveJoint(pUserTracker, userData, nite::JOINT_RIGHT_SHOULDER);
    SaveJoint(pUserTracker, userData, nite::JOINT_LEFT_ELBOW);
    SaveJoint(pUserTracker, userData, nite::JOINT_RIGHT_ELBOW);
    SaveJoint(pUserTracker, userData, nite::JOINT_LEFT_HAND);
    SaveJoint(pUserTracker, userData, nite::JOINT_RIGHT_HAND);
    SaveJoint(pUserTracker, userData, nite::JOINT_TORSO);
    SaveJoint(pUserTracker, userData, nite::JOINT_LEFT_HIP);
    SaveJoint(pUserTracker, userData, nite::JOINT_RIGHT_HIP);
    SaveJoint(pUserTracker, userData, nite::JOINT_LEFT_KNEE);
    SaveJoint(pUserTracker, userData, nite::JOINT_RIGHT_KNEE);
    SaveJoint(pUserTracker, userData, nite::JOINT_LEFT_FOOT);
    SaveJoint(pUserTracker, userData, nite::JOINT_RIGHT_FOOT);
}


void SampleViewer::Display()
{
    if (g_pause)
        return;
    
	nite::UserTrackerFrameRef userTrackerFrame;
    nite::Status rc1 = m_pUserTracker->readFrame(&userTrackerFrame);
	if (rc1 != nite::STATUS_OK) {
		printf("GetNextData failed\n");
		return;
	}
    
	openni::VideoFrameRef depthFrameSide = userTrackerFrame.getDepthFrame();
    int height = depthFrameSide.getHeight();
    int width = depthFrameSide.getWidth();

    if (!label) {
    	label = (int *)malloc(width*height*sizeof(int));
    }
    
    openni::VideoFrameRef depthFrameTop;
    openni::Status rc2 = depthStreamTop.readFrame(&depthFrameTop);
	if (rc2 != openni::STATUS_OK) {
		printf("GetNextData failed\n");
		return;
	}

	if (m_pTexMap == NULL)
	{
		// Texture map init
		m_nTexMapX = MIN_CHUNKS_SIZE(depthFrameSide.getVideoMode().getResolutionX(), TEXTURE_SIZE);
		m_nTexMapY = MIN_CHUNKS_SIZE(depthFrameSide.getVideoMode().getResolutionY(), TEXTURE_SIZE);
		m_pTexMap = new openni::RGB888Pixel[m_nTexMapX * m_nTexMapY];
	}

	const nite::UserMap& userLabels = userTrackerFrame.getUserMap();

	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, GL_WIN_SIZE_X, GL_WIN_SIZE_Y, 0, -1.0, 1.0);

	if (depthFrameSide.isValid() && g_drawDepth)
	{
		calculateHistogram(m_pDepthHistSide, MAX_DEPTH, depthFrameSide);
	}

	memset(m_pTexMap, 0, m_nTexMapX*m_nTexMapY*sizeof(openni::RGB888Pixel));

	float factor[3] = {1, 1, 1};
	// check if we need to draw depth frame to texture
	if (depthFrameSide.isValid() && g_drawDepth)
	{
		const nite::UserId* pLabels = userLabels.getPixels();

		const openni::DepthPixel* pDepthRow = (const openni::DepthPixel*)depthFrameSide.getData();
		openni::RGB888Pixel* pTexRow = m_pTexMap + depthFrameSide.getCropOriginY() * m_nTexMapX;
		int rowSize = depthFrameSide.getStrideInBytes() / sizeof(openni::DepthPixel);
        
		for (int y = 0; y < height; ++y)
		{
			const openni::DepthPixel* pDepth = pDepthRow;
			openni::RGB888Pixel* pTex = pTexRow + depthFrameSide.getCropOriginX();

			for (int x = 0; x < width; ++x, ++pDepth, ++pTex, ++pLabels)
			{
				if (*pDepth != 0)
				{
					if (*pLabels == 0)
					{
						if (!g_drawBackground)
						{
							factor[0] = factor[1] = factor[2] = 0;

						}
						else
						{
							factor[0] = Colors[colorCount][0];
							factor[1] = Colors[colorCount][1];
							factor[2] = Colors[colorCount][2];
						}
					}
					else
					{
						factor[0] = Colors[*pLabels % colorCount][0];
						factor[1] = Colors[*pLabels % colorCount][1];
						factor[2] = Colors[*pLabels % colorCount][2];
					}
//					// Add debug lines - every 10cm
// 					else if ((*pDepth / 10) % 10 == 0)
// 					{
// 						factor[0] = factor[2] = 0;
// 					}

					int nHistValue = m_pDepthHistSide[*pDepth];
					pTex->r = nHistValue*factor[0];
					pTex->g = nHistValue*factor[1];
					pTex->b = nHistValue*factor[2];

					factor[0] = factor[1] = factor[2] = 1;
				}
			}

			pDepthRow += rowSize;
			pTexRow += m_nTexMapX;
		}
	}
    
    const openni::DepthPixel *imgBufferSide = (const openni::DepthPixel *)depthFrameSide.getData();
    const openni::DepthPixel *imgBufferTop = (const openni::DepthPixel *)depthFrameTop.getData();
    calculateHistogram(m_pDepthHistTop, MAX_DEPTH, depthFrameTop);
    imgTop = Mat(depthFrameTop.getHeight(), depthFrameTop.getWidth(), CV_8UC3);
    Mat(depthFrameTop.getHeight(), depthFrameTop.getWidth(), CV_16U, (void *)imgBufferTop).convertTo(depthTop, CV_8U, 1.0/256);
    
    for (int i = 0; i < imgTop.rows; i++) {
        for (int j = 0; j < imgTop.cols; j++) {
            int val = (int)m_pDepthHistTop[imgBufferTop[j + i*imgTop.cols]];
            imgTop.at<Vec3b>(i, j).val[0] = val;
            imgTop.at<Vec3b>(i, j).val[1] = val;
            imgTop.at<Vec3b>(i, j).val[2] = val;
        }
    }
    
    if (g_getBackground)
        bgSubtractor->processImages(depthTop);
    bgSubtractor->getMask(depthTop, mask);
    imshow("Mask", mask);

	glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP_SGIS, GL_TRUE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_nTexMapX, m_nTexMapY, 0, GL_RGB, GL_UNSIGNED_BYTE, m_pTexMap);

	// Display the OpenGL texture map
	glColor4f(1,1,1,1);

	glEnable(GL_TEXTURE_2D);
	glBegin(GL_QUADS);

    // 320x240
	g_nXRes = depthFrameSide.getVideoMode().getResolutionX();
	g_nYRes = depthFrameSide.getVideoMode().getResolutionY();

	// upper left
	glTexCoord2f(0, 0);
	glVertex2f(0, 0);
	// upper right
	glTexCoord2f((float)g_nXRes/(float)m_nTexMapX, 0);
	glVertex2f(GL_WIN_SIZE_X, 0);
	// bottom right
	glTexCoord2f((float)g_nXRes/(float)m_nTexMapX, (float)g_nYRes/(float)m_nTexMapY);
	glVertex2f(GL_WIN_SIZE_X, GL_WIN_SIZE_Y);
	// bottom left
	glTexCoord2f(0, (float)g_nYRes/(float)m_nTexMapY);
	glVertex2f(0, GL_WIN_SIZE_Y);

	glEnd();
	glDisable(GL_TEXTURE_2D);

	const nite::Array<nite::UserData>& users = userTrackerFrame.getUsers();
    float maxSize = -1;
    int maxIdx = -1;
    
    for (int i = 0; i < users.getSize(); ++i) {
        const nite::UserData &user = users[i];
        
        if (!user.isVisible())
            continue;
        
        if (getSize(user) > maxSize) {
            maxSize = getSize(user);
            maxIdx = i;
        }
        //printf("user %d: size=%f\n, lost=%d, new=%d, visible=%d\n",
        //       i, getSize(user), user.isLost(), user.isNew(), user.isVisible());
    }
    
	for (int i = 0; i < users.getSize(); ++i)
	{
		const nite::UserData &user = users[i];

		updateUserState(user, userTrackerFrame.getTimestamp());
		if (user.isNew())
		{
			m_pUserTracker->startSkeletonTracking(user.getId());
			m_pUserTracker->startPoseDetection(user.getId(), nite::POSE_CROSSED_HANDS);
		}
		else if (!user.isLost())
		{
			if (g_drawStatusLabel) {
				DrawStatusLabel(m_pUserTracker, user);
			}
            
            if (g_drawCenterOfMass) {
				DrawCenterOfMass(m_pUserTracker, user);
			}
            
			if (g_drawBoundingBox) {
				DrawBoundingBox(user);
			}

			if (users[i].getSkeleton().getState() == nite::SKELETON_TRACKED && g_drawSkeleton) {
                if (maxIdx == i) {
                    DrawSkeleton(m_pUserTracker, user);
                    sideSkel.setTo(Scalar(0, 0, 0));
                    drawSkeleton(sideSkel, sideJoints);
                    topSkel.setTo(Scalar(0, 0, 0));
                    drawSkeleton(topSkel, topJoints);
                    drawSkeleton(imgTop, topJoints);
                }
			}
		}

        // exit the program after a few seconds if PoseType == POSE_CROSSED_HANDS
		if (m_poseUser == 0 || m_poseUser == user.getId())
		{
			const nite::PoseData& pose = user.getPose(nite::POSE_CROSSED_HANDS);

			if (pose.isEntered())
			{
				// Start timer
				sprintf(g_generalMessage, "In exit pose. Keep it for %d second%s to exit\n", g_poseTimeoutToExit/1000, g_poseTimeoutToExit/1000 == 1 ? "" : "s");
				printf("Counting down %d second to exit\n", g_poseTimeoutToExit/1000);
				m_poseUser = user.getId();
				m_poseTime = userTrackerFrame.getTimestamp();
			}
			else if (pose.isExited())
			{
				memset(g_generalMessage, 0, sizeof(g_generalMessage));
				printf("Count-down interrupted\n");
				m_poseTime = 0;
				m_poseUser = 0;
			}
			else if (pose.isHeld())
			{
				// tick
				if (userTrackerFrame.getTimestamp() - m_poseTime > g_poseTimeoutToExit * 1000)
				{
					printf("Count down complete. Exit...\n");
					Finalize();
					exit(2);
				}
			}
		}
	}

	if (g_drawFrameId)
	{
		DrawFrameId(userTrackerFrame.getFrameIndex());
	}

	if (g_generalMessage[0] != '\0')
	{
		char *msg = g_generalMessage;
		glColor3f(1.0f, 0.0f, 0.0f);
		glRasterPos2i(100, 20);
		glPrintString(GLUT_BITMAP_HELVETICA_18, msg);
	}

    imshow("Side", sideSkel);
    imshow("Top", topSkel);
    imshow("DepthTop", imgTop);
    
    if (!g_getBackground) {
        knnsearch(topJoints, imgBufferTop, mask, labelTop, label, 320, 240);
        drawSkeleton(labelTop, topJoints);
        cv::resize(labelTop, labelTop, Size(), 2, 2);
        imshow("Label", labelTop);
        
        if (g_capture2) {
            // c style
            string path = outDir + "/depth-top" + to_string(nFrame) + ".txt";
            FILE *f = fopen(path.c_str(), "w");
            for (int i = 0; i < width*height; i++) {
                fprintf(f, "%u\n", imgBufferTop[i]);
            }
            fclose(f);
            
            path = outDir + "/depth-side" + to_string(nFrame) + ".txt";
            f = fopen(path.c_str(), "w");
            for (int i = 0; i < width*height; i++) {
                fprintf(f, "%u\n", imgBufferSide[i]);
            }
            fclose(f);
            
            path = outDir + "/joints-top" + to_string(nFrame) + ".txt";
            f = fopen(path.c_str(), "w");
            for (int i = 0; i < N_JOINTS; i++) {
                fprintf(f, "%f, %f, %f, %f, %f\n", topJoints[i][0], topJoints[i][1],
                        topJoints[i][2], topJoints[i][3], topJoints[i][4]);
            }
            fclose(f);
            
            path = outDir + "/joints-side" + to_string(nFrame) + ".txt";
            f = fopen(path.c_str(), "w");
            for (int i = 0; i < N_JOINTS; i++) {
                fprintf(f, "%f, %f, %f, %f, %f\n", sideJoints[i][0], sideJoints[i][1],
                        sideJoints[i][2], sideJoints[i][3], sideJoints[i][4]);
            }
            fclose(f);

            path = outDir + "/label-top" + to_string(nFrame) + ".txt";
            f = fopen(path.c_str(), "w");
            for (int i = 0; i < width*height; i++) {
                fprintf(f, "%d\n", label[i]);
            }
            fclose(f);
            
            path = outDir + "/label-side" + to_string(nFrame) + ".txt";
            f = fopen(path.c_str(), "w");
            const nite::UserId* labelsTop = userLabels.getPixels();
            for (int i = 0; i < width*height; i++) {
                fprintf(f, "%d\n", (int)labelsTop[i]);
            }
            fclose(f);
            
            nFrame++;
        }
    }
    
    // Swap the OpenGL display buffers
	glutSwapBuffers();
}

void SampleViewer::OnKey(unsigned char key, int /*x*/, int /*y*/)
{
	switch (key)
	{
	case 27:
		Finalize();
		exit (1);
	case 's':
		// Draw skeleton?
		g_drawSkeleton = !g_drawSkeleton;
		break;
	case 'l':
		// Draw user status label?
		g_drawStatusLabel = !g_drawStatusLabel;
		break;
	case 'c':
		// Draw center of mass?
		g_drawCenterOfMass = !g_drawCenterOfMass;
		break;
	case 'x':
		// Draw bounding box?
		g_drawBoundingBox = !g_drawBoundingBox;
		break;
	case 'b':
		// Draw background?
		g_drawBackground = !g_drawBackground;
		break;
	case 'd':
		// Draw depth?
		g_drawDepth = !g_drawDepth;
		break;
	case 'f':
		// Draw frame ID
		g_drawFrameId = !g_drawFrameId;
		break;
    case 'p':
        // Pause
        g_pause = !g_pause;
        break;
    case '1':
        // Capture 1 frame
        g_capture1 = !g_capture1;
        break;
    case '2':
        // Capture continuous frames
        g_capture2 = !g_capture2;
        if (g_capture2)
            printf("Start collecting frames\n");
        else
            printf("Done collecting frames\n");
        break;
    case '3':
        // Get background
        g_getBackground = false;
        printf("Done collecting background\n");
        break;
    }
}

openni::Status SampleViewer::InitOpenGL(int argc, char **argv)
{
	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE | GLUT_DEPTH);
	glutInitWindowSize(GL_WIN_SIZE_X, GL_WIN_SIZE_Y);
	glutCreateWindow (m_strSampleName);
	// 	glutFullScreen();
	glutSetCursor(GLUT_CURSOR_NONE);

	InitOpenGLHooks();

	glDisable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);

	glEnableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);

	return openni::STATUS_OK;

}

void SampleViewer::InitOpenGLHooks()
{
	glutKeyboardFunc(glutKeyboard);
    glutDisplayFunc(glutDisplay);
	glutIdleFunc(glutIdle);
}
