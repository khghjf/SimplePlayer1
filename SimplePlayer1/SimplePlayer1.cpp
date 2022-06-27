#include <stdio.h>
#include <cstring>
//从mp4到YUV，再从YUV到屏幕
//ffmpeg：mp4->YUV
//SDL：YUV->屏幕

#define __STDC_CONSTANT_MACROS

extern "C"
{
#include "libavcodec/avcodec.h"		//用于解码
#include "libavformat/avformat.h"	//用于定义数据结构
#include "libswscale/swscale.h"		//用于格式转换
#include "SDL2/SDL.h"
}

//自己定义事件
//Refresh Event
#define REFRESH_EVENT (SDL_USEREVENT + 1)
//Break
#define BREAK_EVENT (SDL_USEREVENT + 2)

//保证线程安全退出
int thread_exit = 0;
int thread_pause = 0;

//线程函数，主函数wait 线程运行  线程函数唤醒主函数
int refresh_video(void* opaque)
{
	thread_exit = 0;
	while (thread_exit == 0)
	{
		//没有暂停才会不断发送refresh事件
		if (thread_pause == 0)
		{
			SDL_Event event;
			event.type = REFRESH_EVENT;
			SDL_PushEvent(&event);	//wait接收后就可以立马运行
			SDL_Delay(80);
		}
	}
	thread_exit = 0;
	//Break  此时thread_exit == 1 没有循环了
	SDL_Event event;
	event.type = BREAK_EVENT;
	SDL_PushEvent(&event);
	return 0;
}

int main(int argc, char* argv[])
{
	char filepath[] = "cuc_ieschool.flv";
	FILE* fp = nullptr;
	fopen_s(&fp, filepath, "rb");
	if (!fp)
	{
		printf("Can't open file !\n");
		return -1;
	}

	//先定义FFmpeg重要的七个数据结构	一定要注意之间的嵌套关系
	AVFormatContext* pFormatCtx;	//记录整个视频文件的基本内容 包含以下所有结构体 嵌套型的
	//AVInputFormat					//每种封装格式对应一个结构体(如FLV、MP4、AVI)
	//AVStream						//整个视频所包含的所有数据流，是个数组
	AVCodecContext* pCodecCtx;		//记录每个视频流(或音频流)的相关信息 
	AVCodecParameters* pCodecCtx_Par;	//新版记录信息

	//新版avcodec_find_decoder返回const类型
	const AVCodec* pCodec;				//记录每个视频流(或音频流)编解码内容(H264)
	AVPacket* packet;				//用来存储一个流的数据(未解码)
	AVFrame* pFrame, * pFrameYUV;	//用来存储解码后的一个流的数据

	int videoindex;		//记录视频流所在的stream下标
	struct SwsContext* img_convert_ctx;	//用于格式转换


	//开始启动FFmpeg
	//av_register_all();  高版本不需要添加了 可以直接使用FFmpeg相关函数
	//avformat_network_init();	使用一些网络功能  运用socket服务

	//初始化结构体  包含AVInputFormat和Stream
	pFormatCtx = avformat_alloc_context();
	//给结构体填入AVInputFormat信息
	if (avformat_open_input(&pFormatCtx, filepath, NULL, NULL) != 0)
	{
		printf("Can't open input stream !\n");
		return -1;
	}
	//给结构体填入stream的信息
	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
	{
		printf("Can't find stream infomation !\n");
		return -1;
	}
	videoindex = -1;	//开始从stream查找h264的下标
	for (int i = 0; i < pFormatCtx->nb_streams; i++)
	{
		//新版本AVStream的封装流参数由codec替换codecpar
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoindex = i;
			break;
		}
	}
	if (videoindex == -1)
	{
		printf("Didn't find a video stream !\n");
		return -1;
	}

	//根据编解码器id初始化编解码器信息  找到视频解码器
	pCodec = avcodec_find_decoder(pFormatCtx->streams[videoindex]->codecpar->codec_id);
	if (pCodec == NULL)
	{
		printf("Codec not found !\n");
		return -1;
	}
	//独立的解码上下文
	pCodecCtx = avcodec_alloc_context3(pCodec);
	avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoindex]->codecpar);
	//将编解码器信息和这个视频流进行关联，初始化AVCodecContext
	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
	{
		printf("Can't open codec !\n");
		return -1;
	}


	//Output Info-----------------------------
	printf("long: %d\n", pFormatCtx->duration);
	printf("format: %s\n", pFormatCtx->iformat->name);
	//每个都是包含多个信息的结构体  层层嵌套
	AVCodecParameters* curAVC = pFormatCtx->streams[videoindex]->codecpar;
	printf("w&h: %d * %d\n", curAVC->width, curAVC->height);
	printf("--------------- File Information ----------------\n");
	//打印关于输入或输出格式的详细信息，例如持续时间，比特率，流，容器，程序，元数据，边数据，编解码器和时基。
	av_dump_format(pFormatCtx, 0, filepath, 0);
	printf("-------------------------------------------------\n");


	//开始为packet和frame分配空间
	packet = (AVPacket*)av_malloc(sizeof(AVPacket));
	//av_frame_alloc其中部分参数默认赋值，原始数据没有分配空间
	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();
	//分配一张图片大小的空间
	//让pFrameYUV的data指向out_buffer(为pFrameYUV的data分配空间)
	pFrameYUV->width = pCodecCtx->width;
	pFrameYUV->height = pCodecCtx->height;
	pFrameYUV->format = AV_PIX_FMT_YUV420P;
	if (av_frame_get_buffer(pFrameYUV, 0) < 0 || av_frame_make_writable(pFrameYUV) < 0)
	{
		printf("Can't allocate the space !\n");
		return -1;
	}





	//开始进行格式转换 将前面格式转换成后面的格式()
	/*
	sws_getContext()	初始化 
	sws_scale()			核心 sws_scale转换包含像素格式转换和缩放拉伸转换，输入输出可以是rgb或yuv中的任意一种。
	sws_freeContext()	释放
	*/
	img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
		pCodecCtx->width , pCodecCtx->height , AV_PIX_FMT_YUV420P , SWS_BICUBIC,NULL,NULL,NULL
		);

	//开始进行SDL初始化 屏幕->渲染器->纹理
	//作用是容SDL is not ready，SDL_SetError 若ready进行一些默认配置
	//各个flags子系统有不同的初始化函数
	if (SDL_Init(SDL_INIT_VIDEO)) {
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return -1;
	}

	SDL_Window* screen;
	int maxScreen_w = 1280;
	int maxScreen_h = 960;
	int pixel_w = pCodecCtx->width;
	int pixel_h = pCodecCtx->height;
	screen = SDL_CreateWindow("SimplePlayer", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		maxScreen_w , maxScreen_h , SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	if (!screen)
	{
		printf("SDL: can't create window - exiting:%s\n", SDL_GetError());
		return -1;
	}
	SDL_Renderer* sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
	Uint32 pixformat = 0;
	//IYUV: Y + U + V  (3 planes)
	//YV12: Y + V + U  (3 planes)
	pixformat = SDL_PIXELFORMAT_IYUV;	//表示IYUV这四个字符，每个1B 设置纹理的格式
	//定义纹理
	SDL_Texture* sdlTexture = SDL_CreateTexture(sdlRenderer, pixformat, SDL_TEXTUREACCESS_STREAMING, pixel_w, pixel_h);
	//设置显示视频的大小
	SDL_Rect sdlRect;
	sdlRect.x = 0;
	sdlRect.y = 0;
	sdlRect.w = pixel_w;
	sdlRect.h = pixel_h;
	SDL_Event sdlEvent;
	bool quit = true;
	SDL_Point leftPoint;

	int cnt = 1;
	//开始播放  先只播放一遍

	//运行线程
	SDL_Thread* refresh_thread = SDL_CreateThread(refresh_video, NULL, NULL);
	SDL_Event event;
	//
	while (av_read_frame(pFormatCtx, packet) >= 0)
	{
		//从数据中找到H264解码成YUV 之后在进行事件判断
		if (packet->stream_index == videoindex)
		{
			//此时为相应的H264包
			//1.先将数据包发送给解码器 返回0成功
			if (avcodec_send_packet(pCodecCtx, packet) != 0)
			{
				printf("当前解码器满的，无法接收新的数据包!\n");
				return -1;
			}
			//2.本次循环中 , 将 packet 丢到解码器中解码完毕后 , 就可以释放 packet 内存了
			//av_packet_free(&packet);
			//3.接收并解码数据包 , 存放在 AVFrame 中
			//AVFrame* curFrame = av_frame_alloc();
			//4.解码器中将数据包解码后 , 存放到 AVFrame * 中 , 这里将其取出并解码
			//  返回 AVERROR(EAGAIN) : 当前状态没有输出 , 需要输入更多数据
			//  返回 AVERROR_EOF : 解码器中没有数据 , 已经读取到结尾
			//  返回 AVERROR(EINVAL) : 解码器没有打开
			//一直返回-11 此时编码器需要新的输入数据才能返回输出
			int res = avcodec_receive_frame(pCodecCtx, pFrame);
			/*if (res != 0)
			{
				printf("解码失败！%d\n" , res);
				return -1;
			}*/

			//现在curFrame存放未处理的YUV，通过sws_scale来处理
			sws_scale(img_convert_ctx, (const uint8_t* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height,
				pFrameYUV->data, pFrameYUV->linesize);

			//写入YUV数据  这里linesize只有wide的数量，还需要乘以height
			/*fwrite(pFrameYUV->data[0], 1, pFrameYUV->linesize[0] * pixel_h, output_YUV);
			fwrite(pFrameYUV->data[1], 1, pFrameYUV->linesize[1] * pixel_h / 2, output_YUV);
			fwrite(pFrameYUV->data[2], 1, pFrameYUV->linesize[2] * pixel_h / 2, output_YUV);*/


			//有时正常  有时出错？？？为啥最多到70帧 上面的却可以读完
			/*
			以对内在操作的过程中，所写的地址超出了，所分配内在的边界
				memcpy的时候， copy的大小超出了目标数组的大小  地址超出了  不知道是哪个
			不需要重新申请buffer
			*/
			//memcpy(buffer_temp, pFrameYUV->data[0], pixel_h* pixel_w * 3 / 2);
			//开始输出
			//但是经过比较sdl输出的yuv比写入的yuv文件要花  ？？？
			//使用SDL_UpdateYUVTexture 来替换 SDL_UpdateTexture 图像不会花
			SDL_WaitEvent(&event);
			if (event.type == REFRESH_EVENT)
			{
				//这里有问题
				printf("x: %d    y: %d     w: %d    h: %d\n", sdlRect.x, sdlRect.y, sdlRect.w, sdlRect.h);
				printf("here0 ");
				SDL_UpdateYUVTexture(sdlTexture, NULL,
					pFrameYUV->data[0], pFrameYUV->linesize[0],
					pFrameYUV->data[1], pFrameYUV->linesize[1],
					pFrameYUV->data[2], pFrameYUV->linesize[2]);
				printf("here1 ");
				SDL_RenderClear(sdlRenderer);
				printf("here2 ");
				SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
				printf("here3 ");
				SDL_RenderPresent(sdlRenderer);
				printf("here4 \n");
			}
			else if (event.type == SDL_QUIT)
			{
				thread_exit = 1;
			}
			else if (event.type == BREAK_EVENT)
			{
				break;
			}
			//键盘事件
			else if (event.type == SDL_KEYDOWN)
			{
				if (event.key.keysym.sym == SDLK_SPACE)
				{
					thread_pause = !thread_pause;
				}/*
				else if (event.key.keysym.sym == SDLK_ESCAPE)
				{
					thread_exit = 1;
				}*/
			}
			//鼠标点击事件
			else if (event.type == SDL_MOUSEBUTTONDOWN)
			{
				//SDL_BUTTON_LEFT 左键点击
				if (event.button.button == SDL_BUTTON_LMASK)
				{
					leftPoint.x = event.button.x;
					leftPoint.y = event.button.y;
				}
			}
			else if (event.type == SDL_MOUSEMOTION)
			{
				if (event.button.button == SDL_BUTTON_LMASK)
				{
					//相对移动
					SDL_Point curLeftPoint;
					curLeftPoint.x = event.button.x;
					curLeftPoint.y = event.button.y;

					sdlRect.x += curLeftPoint.x - leftPoint.x;
					sdlRect.y += curLeftPoint.y - leftPoint.y;
					leftPoint = curLeftPoint;
				}
			}
			//鼠标滑轮事件
			else if (event.type == SDL_MOUSEWHEEL)
			{
				//往上滚
				if (event.wheel.y > 0)
				{
					sdlRect.w *= 1.1;
					sdlRect.h *= 1.1;
				}
				//往下滚
				if (event.wheel.y < 0)
				{
					sdlRect.w /= 1.1;
					sdlRect.h /= 1.1;
				}
			}
		}
	}
	
	av_packet_free(&packet);

	//sdl文件关闭
	SDL_Quit();
	//ffmpeg文件关闭防止泄露
	if (pFrameYUV) {
		av_frame_free(&pFrameYUV);
		pFrameYUV = nullptr;
	}

	fclose(fp);
	sws_freeContext(img_convert_ctx);
	av_frame_free(&pFrameYUV);
	av_frame_free(&pFrame);
	avcodec_close(pCodecCtx);
	avformat_close_input(&pFormatCtx);

	return 0;
}