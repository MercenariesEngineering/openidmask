/*

             *
            ***
           *****
   *********************       Mercenaries Engineering SARL
     *****************              Copyright (C) 2016
       *************
         *********        http://www.mercenaries-engineering.com
        ***********
       ****     ****
      **           **

*/

#include <set>
#include <openidmask/Mask.h>
#include <openidmask/Query.h>
#include <ImfDeepScanLineInputFile.h>
#include "instance.h"
#include "ofxUtilities.h"

using namespace Imf;
using namespace Imath;

#if defined __APPLE__ || defined linux || defined __FreeBSD__
#  define EXPORT __attribute__((visibility("default")))
#elif defined _WIN32
#  define EXPORT OfxExport
#else
#  error Not building on your operating system quite yet
#endif

// pointers64 to various bits of the host
OfxHost					*gHost = NULL;
OfxImageEffectSuiteV1	*gEffectHost = NULL;
OfxPropertySuiteV1		*gPropHost = NULL;
OfxParameterSuiteV1		*gParamHost = NULL;
OfxMultiThreadSuiteV1	*gThreadHost = NULL;
OfxInteractSuiteV1		*gInteractHost = NULL;
OfxMemorySuiteV1		*gMemoryHost = NULL;
OfxMessageSuiteV1		*gMessageSuite = NULL;

// Convinience wrapper to get private data 
static Instance *getInstanceData(OfxImageEffectHandle effect)
{
	Instance *instance = (Instance *) ofxuGetEffectInstanceData(effect);
	return instance;
}

//  instance construction
static OfxStatus createInstance(OfxImageEffectHandle effect)
{
	// get a pointer to the effect properties
	OfxPropertySetHandle effectProps;
	gEffectHost->getPropertySet(effect, &effectProps);

	// get a pointer to the effect's parameter set
	OfxParamSetHandle paramSet;
	gEffectHost->getParamSet(effect, &paramSet);

	// make my private instance data
	Instance *instance = new Instance;

	// cache away param handles
	gParamHost->paramGetHandle(paramSet, "file", &instance->fileParam, 0);
	gParamHost->paramGetHandle(paramSet, "pattern", &instance->patternParam, 0);
	gParamHost->paramGetHandle(paramSet, "colors", &instance->colorsParam, 0);

	// cache away clip handles
	gEffectHost->clipGetHandle(effect, kOfxImageEffectOutputClipName, &instance->outputClip, 0);

	// set my private instance data
	gPropHost->propSetPointer(effectProps, kOfxPropInstanceData, 0, (void *) instance);

	return kOfxStatOK;
}

// instance destruction
static OfxStatus destroyInstance(OfxImageEffectHandle effect)
{
	// get my instance data
	Instance *instance = getInstanceData(effect);

	// and delete it
	if(instance)
		delete instance;
	return kOfxStatOK;
}

// tells the host what region we are capable of filling
OfxStatus getSpatialRoD(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs)
{
	OfxStatus status = kOfxStatOK;

	// retrieve any instance data associated with this effect
	Instance *instance = getInstanceData(effect);

	// Read the mas now
	const char *filename;
	gParamHost->paramGetValue(instance->fileParam, &filename);

	try 
	{
		DeepScanLineInputFile file (filename);
		const Header& header = file.header();
		const Box2i displayWindow = header.displayWindow();
		const Box2i dataWindow = header.dataWindow();
		const int w = dataWindow.max.x+1-dataWindow.min.x;
		const int h = dataWindow.max.y+1-dataWindow.min.y;
		const int imageH = displayWindow.max.y+1;
		const int xMin = dataWindow.min.x;
		const int yMin = imageH-dataWindow.max.y-1;
		const double res[] = {(double)xMin, (double)yMin, (double)(xMin+w), (double)(yMin+h)};
		gPropHost->propSetDoubleN(outArgs, kOfxImageEffectPropRegionOfDefinition, 4, res);
	}
	catch (std::exception &)
	{
		status = kOfxStatFailed;
	}

	return status;
}

// look up a pixel in the image, does bounds checking to see if it is in the image rectangle
template <class PIX> 
inline PIX *pixelAddress(PIX *img, OfxRectI rect, int x, int y, int bytesPerLine)
{  
	if(x < rect.x1 || x >= rect.x2 || y < rect.y1 || y > rect.y2)
		return 0;
	PIX *pix = (PIX *) (((char *) img) + (y - rect.y1) * bytesPerLine);
	pix += x - rect.x1;  
	return pix;
}

// base class to process images with
class Processor 
{
public :
	Processor(OfxImageEffectHandle eff, OfxPointD rs, void *dst, 
		OfxRectI dRect, int dBytesPerLine, OfxRectI  win, openidmask::Query &query, bool colors)
		: effect(eff)
		, renderScale(rs)
		, dstV(dst)
		, dstRect(dRect)
		, window(win)
		, dstBytesPerLine(dBytesPerLine)
		, Query (query)
		, Colors (colors)
	{}

	static void multiThreadProcessing(unsigned int threadId, unsigned int nThreads, void *arg);
	void doProcessing(OfxRectI window);
	void process(void);

protected :
	OfxImageEffectHandle effect;
	OfxPointD		renderScale;
	void *dstV; 
	OfxRectI dstRect;
	OfxRectI  window;
	int dstBytesPerLine;
	openidmask::Query &Query;
	bool	Colors;
};

// function call once for each thread by the host
void Processor::multiThreadProcessing(unsigned int threadId, unsigned int nThreads, void *arg)
{	
	Processor *proc = (Processor *) arg;

	// slice the y range into the number of threads it has
	unsigned int dy = proc->window.y2 - proc->window.y1;
  
	unsigned int y1 = proc->window.y1 + threadId * dy/nThreads;
	unsigned int y2 = proc->window.y1 + std::min((threadId + 1) * dy/nThreads, dy);

	OfxRectI win = proc->window;
	win.y1 = y1; win.y2 = y2;

	// and render that thread on each
	proc->doProcessing(win);
}

// function to kick off rendering across multiple CPUs
void Processor::process(void)
{
	unsigned int nThreads;
	gThreadHost->multiThreadNumCPUs(&nThreads);
	gThreadHost->multiThread(multiThreadProcessing, nThreads, (void *) this);
}

inline float halton (float base, int id)
{
	float result = 0;
	float f = 1;
	float i = (float)id;
	while (i > 0)
	{
		f = f / base;
		result = result + f * fmodf (i, base);
		i = floorf (i / base);
	}
	return result;
}

inline OfxRGBColourF haltonColors (int id)
{
	return {halton (2, id), halton (3, id), halton (5, id)};
}

void Processor::doProcessing(OfxRectI procWindow)
{
	OfxRGBAColourF *dst = (OfxRGBAColourF*)dstV;

	for(int y = procWindow.y1; y < procWindow.y2; y++) 
	{
		// Convert to top-bottom
		const std::pair<int,int> size = Query.TheMask->getSize ();
		const int _y = size.second-(int)((double)y/renderScale.y)-1;
		if(gEffectHost->abort(effect)) break;

		OfxRGBAColourF *dstPix = pixelAddress(dst, dstRect, procWindow.x1, y, dstBytesPerLine);

		for(int x = procWindow.x1; x < procWindow.x2; x++)
		{
			const int _x = (int)((double)x/renderScale.x);
			dstPix->a = 1;

			// False colors ?
			if (Colors)
			{
				dstPix->r = dstPix->g = dstPix->b = 0;
				const int n = Query.TheMask->getSampleN (_x, _y);
				for (int s = 0; s < n; ++s)
				{
					const openidmask::Sample &sample = Query.TheMask->getSample (_x, _y, s);
					const OfxRGBColourF c = Query.isSelected (sample.Id) ? OfxRGBColourF{1,1,1} : haltonColors (sample.Id);
					dstPix->r += powf (c.r, 1.f/0.3f)*sample.Coverage;
					dstPix->g += powf (c.g, 1.f/0.3f)*sample.Coverage;
					dstPix->b += powf (c.b, 1.f/0.3f)*sample.Coverage;
				}
			}
			else
			{
				const float c = Query.getCoverage (_x, _y);
				dstPix->r = dstPix->g = dstPix->b = c;
			}
			dstPix++;
		}
	}
}

// the process code  that the host sees
static OfxStatus render(OfxImageEffectHandle effect,
						OfxPropertySetHandle inArgs,
						OfxPropertySetHandle /*outArgs*/)
{
	// get the render window and the time from the inArgs
	OfxTime time;
	OfxRectI renderWindow;
	OfxStatus status = kOfxStatOK;

	gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);
	gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &renderWindow.x1);

	// retrieve any instance data associated with this effect
	Instance *instance = getInstanceData(effect);

	// property handles and members of each image
	// in reality, we would put this in a struct as the C++ support layer does
	OfxPropertySetHandle outputImg = NULL;
	int srcRowBytes = 0, dstRowBytes, dstBitDepth;
	bool dstIsAlpha;
	OfxRectI dstRect, srcRect = {0};
	void *dst;

	try 
	{
		outputImg = ofxuGetImage(instance->outputClip, time, dstRowBytes, dstBitDepth, dstIsAlpha, dstRect, dst);
		if(outputImg == NULL)
			throw OfxuNoImageException();

		// get the render scale
		OfxPointD renderScale;
		gPropHost->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, &renderScale.x);
		const char *filename;
		gParamHost->paramGetValue(instance->fileParam, &filename);
		const char *pattern;
		gParamHost->paramGetValue(instance->patternParam, &pattern);
		int colors;
		gParamHost->paramGetValue(instance->colorsParam, &colors);

		// Split the string in strings
		std::set<std::string> patterns;
		std::string line;
		while (true)
		{
			const char c = *(pattern++);
			if (c == '\n' || c == '\r' || c == '\0')
			{
				if (!line.empty())
				{
					patterns.insert (line);
					line.clear ();
				}
			}
			else
				line += c;
			if (c == '\0')
				break;
		}

		try
		{
			if (instance->LastMaskFilename != filename)
			{
				instance->Mask.read (filename);
				instance->LastMaskFilename = filename;
			}
			auto match = [&patterns] (const char *name)
			{
				for (auto &pattern : patterns)
					if (strstr(name, pattern.c_str()) != NULL)
						return true;
				return false;
			};
			openidmask::Query query (&instance->Mask, match);

			// do the rendering
			Processor fred (effect, renderScale, dst, dstRect, dstRowBytes, renderWindow, query, colors != 0);
			fred.process();
		}
		catch (std::exception &)
		{
			status = kOfxStatFailed;
		}

	}
	catch(OfxuNoImageException &) 
	{
		// if we were interrupted, the failed fetch is fine, just return kOfxStatOK
		// otherwise, something wierd happened
		if(!gEffectHost->abort(effect)) 
			status = kOfxStatFailed;
	}

	// release the data pointers
	if(outputImg)
		gEffectHost->clipReleaseImage(outputImg);

	return status;
}

// Set our clip preferences 
static OfxStatus getClipPreferences(OfxImageEffectHandle effect, OfxPropertySetHandle /*inArgs*/, OfxPropertySetHandle outArgs)
{
	// retrieve any instance data associated with this effect
	Instance *instance = getInstanceData(effect);

	return kOfxStatOK;
}

//  describe the plugin in context
static OfxStatus describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
{
	OfxPropertySetHandle clipProps;
	// define the single output clip in both contexts
	gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &clipProps);
	gPropHost->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);

	// define the parameters for this context

	// get a pointer to the effect's parameter set
	OfxParamSetHandle paramSet;
	gEffectHost->getParamSet(effect, &paramSet);

	// our 2 corners are normalised spatial 2D doubles
	OfxPropertySetHandle paramProps;

	// The input filename
	gParamHost->paramDefine(paramSet, kOfxParamTypeString, "file", &paramProps);
	gPropHost->propSetString(paramProps, kOfxParamPropHint, 0, "The openidmask file");
	gPropHost->propSetString(paramProps, kOfxParamPropScriptName, 0, "file");
	gPropHost->propSetString(paramProps, kOfxPropLabel, 0, "File");
	gPropHost->propSetString(paramProps, kOfxParamPropStringMode, 0, kOfxParamStringIsFilePath);

	// The mask pattern
	gParamHost->paramDefine(paramSet, kOfxParamTypeString, "pattern", &paramProps);
	gPropHost->propSetString(paramProps, kOfxParamPropHint, 0, "The object selection pattern");
	gPropHost->propSetString(paramProps, kOfxParamPropScriptName, 0, "pattern");
	gPropHost->propSetString(paramProps, kOfxPropLabel, 0, "Pattern");
	gPropHost->propSetString(paramProps, kOfxParamPropStringMode, 0, kOfxParamStringIsMultiLine);

	// The mask pattern
	gParamHost->paramDefine(paramSet, kOfxParamTypeBoolean, "colors", &paramProps);
	gPropHost->propSetString(paramProps, kOfxParamPropHint, 0, "Show the image with false colors");
	gPropHost->propSetString(paramProps, kOfxParamPropScriptName, 0, "colors");
	gPropHost->propSetString(paramProps, kOfxPropLabel, 0, "Colors");
	gPropHost->propSetString(paramProps, kOfxParamPropStringMode, 0, kOfxParamStringIsMultiLine);

	return kOfxStatOK;
}

// the plugin's description routine
static OfxStatus describe(OfxImageEffectHandle effect)
{
	// first fetch the host APIs, this cannot be done before this call
	OfxStatus stat;
	if((stat = ofxuFetchHostSuites()) != kOfxStatOK)
		return stat;

	// get a pointer to the effect's set of properties
	OfxPropertySetHandle effectProps;
	gEffectHost->getPropertySet(effect, &effectProps);

	// set the bit depths the plugin can handle
	gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthFloat);

	// set some labels and the group it belongs to
	gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "openidmask");
	gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "Image");

	// define the contexts we can be used in
	gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextGeneral);

	// set the property that is the overlay's main entry point for the plugin
	extern OfxStatus overlayMain(const char *action,  const void *handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle /*outArgs*/);
	gPropHost->propSetPointer(effectProps, kOfxImageEffectPluginPropOverlayInteractV1, 0,  (void *) overlayMain);

	return kOfxStatOK;
}

// The main function
static OfxStatus pluginMain(const char *action,  const void *handle, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs)
{
	try 
	{
		// cast to appropriate type
		OfxImageEffectHandle effect = (OfxImageEffectHandle ) handle;

		if(strcmp(action, kOfxActionDescribe) == 0)
			return describe(effect);
		else if(strcmp(action, kOfxImageEffectActionDescribeInContext) == 0)
			return describeInContext(effect, inArgs);
		else if(strcmp(action, kOfxActionCreateInstance) == 0)
			return createInstance(effect);
		else if(strcmp(action, kOfxActionDestroyInstance) == 0)
			return destroyInstance(effect);
		else if(strcmp(action, kOfxImageEffectActionRender) == 0)
			return render(effect, inArgs, outArgs);
		else if(strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0)
			return getSpatialRoD(effect, inArgs, outArgs);
		else if(strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0)
			return getClipPreferences(effect, inArgs, outArgs);
	} 
	catch (std::bad_alloc)
	{
		// catch memory
		//std::cout << "OFX Plugin Memory error." << std::endl;
		return kOfxStatErrMemory;
	}
	catch ( const std::exception &)
	{
		// standard exceptions
		//std::cout << "OFX Plugin error: " << e.what() << std::endl;
		return kOfxStatErrUnknown;
	} 
	catch (int err) 
	{
		// ho hum, gone wrong somehow
		return err;
	}
	catch ( ... ) 
	{
		// everything else
		//std::cout << "OFX Plugin error" << std::endl;
		return kOfxStatErrUnknown;
	}
    
	// other actions to take the default value
	return kOfxStatReplyDefault;
}

// function to set the host structure
static void setHostFunc(OfxHost *hostStruct)
{
	gHost = hostStruct;
}

static OfxPlugin basicPlugin = 
{
	kOfxImageEffectPluginApi,
	1,
	"fr.mercenariesengineering.openidmask",
	1,
	0,
	setHostFunc,
	pluginMain
};

EXPORT OfxPlugin *OfxGetPlugin(int nth)
{
	if(nth == 0)
		return &basicPlugin;
	return 0;
}
 
EXPORT int OfxGetNumberOfPlugins(void)
{
	return 1;
}