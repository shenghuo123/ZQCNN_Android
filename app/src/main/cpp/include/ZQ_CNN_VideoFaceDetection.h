#ifndef _ZQ_CNN_VIDEO_FACE_DETECTION_H_
#define _ZQ_CNN_VIDEO_FACE_DETECTION_H_
#pragma once

#include "ZQ_CNN_Net.h"
#include "ZQ_CNN_BBoxUtils.h"
#include "ZQ_CNN_MTCNN.h"
#include "ZQ_CNN_CascadeOnet.h"
#include "ZQlib/ZQ_SVD.h"
#include <vector>
namespace ZQ
{
	class ZQ_CNN_VideoFaceDetection
	{
		using string = std::string;
	public:
		enum VFD_MSG {
			VFD_MSG_MAX_TRACE_NUM,
			VFD_MSG_WEIGHT_DECAY,
			VFD_MSG_FORCE_FIRST_FRAME
		};

		ZQ_CNN_VideoFaceDetection() 
		{
			max_trace_num = 4;
			weight_decay = 0;
			show_debug_info = false;
			is_first_frame = true;
			thread_num = 1;
			has_lnet106 = false;
			key_cooldown = 50;
		}
		~ZQ_CNN_VideoFaceDetection() {}

	private:
		int max_trace_num;
		float weight_decay;
		bool show_debug_info;
		float othresh;
		bool is_first_frame;
		int thread_num;
		ZQ_CNN_MTCNN mtcnn;
		std::vector<ZQ_CNN_CascadeOnet> cascade_Onets;
		bool has_lnet106;
		std::vector<ZQ_CNN_Net> lnets106;
		int key_cooldown;
		int cur_key_cooldown;
		std::vector<std::vector<ZQ_CNN_BBox106> > trace;
		ZQ_CNN_Tensor4D_NHW_C_Align128bit input, lnet106_image;
		int lnet106_size;
	public:
		void TurnOnShowDebugInfo() { show_debug_info = true; }
		void TurnOffShowDebugInfo() { show_debug_info = false; }

		bool Init(const string& pnet_param, const string& pnet_model, const string& rnet_param, const string& rnet_model,
			const string& onet_param, const string& onet_model, int thread_num = 1,
			bool has_lnet106 = false, const string& lnet106_param = "", const std::string& lnet106_model = "")
		{
			if (!mtcnn.Init(pnet_param, pnet_model, rnet_param, rnet_model, onet_param, onet_model, thread_num, has_lnet106, lnet106_param, lnet106_model))
				return false;
			this->thread_num = __max(1,thread_num);
			cascade_Onets.resize(this->thread_num);
			for (int i = 0; i < cascade_Onets.size(); i++)
			{
				if (!cascade_Onets[i].Init(onet_param, onet_model, onet_param, onet_model, onet_param, onet_model))
					return false;
			}
			this->has_lnet106 = has_lnet106;
			if (has_lnet106)
			{
				lnets106.resize(this->thread_num);
				for (int i = 0; i < lnets106.size(); i++)
				{
					if (!lnets106[i].LoadFrom(lnet106_param, lnet106_model,true,1e-9,true))
						return false;
				}
				int C, H, W;
				lnets106[0].GetInputDim(C, H, W);
				lnet106_size = H;
			}
			return true;
		}

		void SetPara(int w, int h, int min_face_size = 60, float pthresh = 0.6, float rthresh = 0.7, float othresh = 0.7,
			float nms_pthresh = 0.6, float nms_rthresh = 0.7, float nms_othresh = 0.7, float scale_factor = 0.709,
			int pnet_overlap_thresh_count = 3, int pnet_size = 20, int pnet_stride = 4, int key_cooldown = 50)
		{
			this->key_cooldown = key_cooldown;
			mtcnn.SetPara(w, h, min_face_size, pthresh, rthresh, othresh, nms_pthresh, nms_rthresh, nms_othresh, 
				scale_factor, pnet_overlap_thresh_count, pnet_size, pnet_stride, true, true, 1.0);
			this->othresh = othresh;
		}

		void Message(const VFD_MSG msg, double val)
		{
			switch (msg)
			{
			case VFD_MSG_FORCE_FIRST_FRAME:
				is_first_frame = true;
				break;
			case VFD_MSG_MAX_TRACE_NUM:
				max_trace_num = __max(0, val);
				break;
			case VFD_MSG_WEIGHT_DECAY:
				weight_decay = val;
				break;
			}
		}

		bool Find(const unsigned char* bgr_img, int _width, int _height, int _widthStep, std::vector<ZQ_CNN_BBox106>& results)
		{
			if (!input.ConvertFromBGR(bgr_img, _width, _height, _widthStep))
				return false;

			results.clear();
			
			if (is_first_frame)
			{
				if (!mtcnn.Find106(input, results))
					return false;
				_recompute_bbox(results);
				int cur_box_num = results.size();
				trace.clear();
				trace.resize(cur_box_num);
				for (int i = 0; i < cur_box_num; i++)
					trace[i].push_back(results[i]);
				is_first_frame = results.size() == 0;
				cur_key_cooldown = key_cooldown;
			}
			else
			{
				/**********   Stage 1: detect around the old positions         ************/

				std::vector<ZQ_CNN_BBox> boxes;
				std::vector<ZQ_CNN_BBox> last_boxes(trace.size());
				for (int i = 0; i < trace.size(); i++)
				{
					last_boxes[i].col1 = trace[i][0].col1;
					last_boxes[i].col2 = trace[i][0].col2;
					last_boxes[i].row1 = trace[i][0].row1;
					last_boxes[i].row2 = trace[i][0].row2;
					last_boxes[i].exist = true;
				}
				ZQ_CNN_BBoxUtils::_square_bbox(last_boxes, input.GetW(), input.GetH());
				std::vector<int> good_idx;
				std::vector<ZQ_CNN_OrderScore> orders;
				std::vector<ZQ_CNN_BBox> tmp_boxes;
				ZQ_CNN_OrderScore tmp_order;
				int ori_count = 0;
				for (int i = 0; i < last_boxes.size(); i++)
				{
					int nIters = 3;
					if (!cascade_Onets[0].Find(input, last_boxes[i].col1, last_boxes[i].row1, last_boxes[i].col2, last_boxes[i].row2, tmp_boxes, nIters))
						return false;
					if (tmp_boxes[nIters - 1].score >= othresh)
					{
						good_idx.push_back(i);
						tmp_boxes[nIters - 1].exist = true;
						tmp_boxes[nIters - 1].score = 2.0;
						tmp_order.score = 2.0;
						tmp_order.oriOrder = ori_count ++ ;
						boxes.push_back(tmp_boxes[nIters - 1]);
						orders.push_back(tmp_order);
					}
				}

				/**********   Stage 2: detect globally         ************/
				if (cur_key_cooldown <= 0)
				{
					std::vector<ZQ_CNN_BBox> tmp_boxes;
					cur_key_cooldown = key_cooldown;
					mtcnn.Find(input, tmp_boxes);
					for (int j = 0; j < tmp_boxes.size(); j++)
					{
						tmp_order.oriOrder = ori_count++;
						tmp_order.score = tmp_boxes[j].score;
						boxes.push_back(tmp_boxes[j]);
						orders.push_back(tmp_order);
						good_idx.push_back(-1);
					}
				}

				/**********   Stage 3: nms         ************/
				std::vector<int> keep_orders;
				_nms(boxes, orders, keep_orders, 0.5, "Union");

				std::vector<int> old_good_idx = good_idx;
				std::vector<ZQ_CNN_BBox> old_boxes = boxes;
				good_idx.clear();
				boxes.clear();
				for (int i = 0; i < keep_orders.size(); i++)
				{
					good_idx.push_back(old_good_idx[keep_orders[i]]);
					boxes.push_back(old_boxes[keep_orders[i]]);
				}

				/**********   Stage 4: get 106 landmark         ************/
				_Lnet106_stage(boxes, results);

				/**********   Stage 5: filtering        ************/
				int cur_box_num = results.size();
				std::vector<std::vector<ZQ_CNN_BBox106> > old_trace(trace);
				trace.clear();
				trace.resize(cur_box_num);
				for (int i = 0; i < cur_box_num; i++)
				{
					trace[i].push_back(results[i]);
					if (good_idx[i] >= 0)
					{
						std::vector<ZQ_CNN_BBox106>& tmp_old_trace = old_trace[good_idx[i]];
						for (int j = 0; j < tmp_old_trace.size() && j < max_trace_num; j++)
							trace[i].push_back(tmp_old_trace[j]);
					}
				}
				_filtering(trace, results);

				/**********   Stage 6: update bbox        ************/
				_recompute_bbox(results);
				
				if (results.size() == 0)
					is_first_frame = true;
			}
			cur_key_cooldown -- ;
			return true;
		}

	private:

		static bool _cmp_score(const ZQ_CNN_OrderScore& lsh, const ZQ_CNN_OrderScore& rsh)
		{
			return lsh.score < rsh.score;
		}

		static void _nms(const std::vector<ZQ_CNN_BBox> &ori_boundingBox, const std::vector<ZQ_CNN_OrderScore> &orderScore, std::vector<int>& keep_orders,
			const float overlap_threshold, const std::string& modelname = "Union")
		{
			std::vector<ZQ_CNN_BBox> boundingBox = ori_boundingBox;
			std::vector<ZQ_CNN_OrderScore> bboxScore = orderScore;
			if (boundingBox.empty() || overlap_threshold >= 1.0)
			{
				return;
			}
			std::vector<int> heros;
			//sort the score
			
			sort(bboxScore.begin(), bboxScore.end(), _cmp_score);

			int order = 0;
			float IOU = 0;
			float maxX = 0;
			float maxY = 0;
			float minX = 0;
			float minY = 0;
			while (bboxScore.size() > 0)
			{
				order = bboxScore.back().oriOrder;
				bboxScore.pop_back();
				if (order < 0)continue;
				heros.push_back(order);
				boundingBox[order].exist = false;//delete it
				int box_num = boundingBox.size();
				for (int num = 0; num < box_num; num++)
				{
					if (boundingBox[num].exist)
					{
						//the iou
						maxY = __max(boundingBox[num].row1, boundingBox[order].row1);
						maxX = __max(boundingBox[num].col1, boundingBox[order].col1);
						minY = __min(boundingBox[num].row2, boundingBox[order].row2);
						minX = __min(boundingBox[num].col2, boundingBox[order].col2);
						//maxX1 and maxY1 reuse 
						maxX = __max(minX - maxX + 1, 0);
						maxY = __max(minY - maxY + 1, 0);
						//IOU reuse for the area of two bbox
						IOU = maxX * maxY;
						float area1 = boundingBox[num].area;
						float area2 = boundingBox[order].area;
						if (!modelname.compare("Union"))
							IOU = IOU / (area1 + area2 - IOU);
						else if (!modelname.compare("Min"))
						{
							IOU = IOU / __min(area1, area2);
						}
						if (IOU > overlap_threshold)
						{
							boundingBox[num].exist = false;
							for (std::vector<ZQ_CNN_OrderScore>::iterator it = bboxScore.begin(); it != bboxScore.end(); it++)
							{
								if ((*it).oriOrder == num)
								{
									(*it).oriOrder = -1;
									break;
								}
							}
						}
					}
				}
			}

			
			for (int i = 0; i < heros.size(); i++)
			{
				boundingBox[heros[i]].exist = true;
			}
			//clear exist= false;
			for (int i = boundingBox.size() - 1; i >= 0; i--)
			{
				if (!boundingBox[i].exist)
				{
					boundingBox.erase(boundingBox.begin() + i);
				}
			}
			keep_orders = heros;
		}

		bool _Lnet106_stage(std::vector<ZQ_CNN_BBox>& thirdBbox, std::vector<ZQ_CNN_BBox106>& resultBbox)
		{
			double t4 = omp_get_wtime();
			std::vector<ZQ_CNN_BBox> fourthBbox;
			std::vector<ZQ_CNN_BBox>::iterator it = thirdBbox.begin();
			std::vector<int> src_off_x, src_off_y, src_rect_w, src_rect_h;
			int l_count = 0;
			for (; it != thirdBbox.end(); it++)
			{
				if ((*it).exist)
				{
					int off_x = it->col1;
					int off_y = it->row1;
					int rect_w = it->col2 - off_x;
					int rect_h = it->row2 - off_y;

					l_count++;
					fourthBbox.push_back(*it);
				}
			}
			std::vector<ZQ_CNN_BBox> copy_fourthBbox = fourthBbox;
			ZQ_CNN_BBoxUtils::_square_bbox(copy_fourthBbox, input.GetW(), input.GetH());
			for (it = copy_fourthBbox.begin(); it != copy_fourthBbox.end(); ++it)
			{
				int off_x = it->col1;
				int off_y = it->row1;
				int rect_w = it->col2 - off_x;
				int rect_h = it->row2 - off_y;
				src_off_x.push_back(off_x);
				src_off_y.push_back(off_y);
				src_rect_w.push_back(rect_w);
				src_rect_h.push_back(rect_h);
			}

			int batch_size = 1;
			int per_num = ceil((float)l_count / thread_num);
			int need_thread_num = thread_num;
			if (per_num > batch_size)
			{
				need_thread_num = ceil((float)l_count / batch_size);
				per_num = batch_size;
			}

			std::vector<ZQ_CNN_Tensor4D_NHW_C_Align128bit> task_lnet_images(need_thread_num);
			std::vector<std::vector<int> > task_src_off_x(need_thread_num);
			std::vector<std::vector<int> > task_src_off_y(need_thread_num);
			std::vector<std::vector<int> > task_src_rect_w(need_thread_num);
			std::vector<std::vector<int> > task_src_rect_h(need_thread_num);
			std::vector<std::vector<ZQ_CNN_BBox106> > task_fourthBbox(need_thread_num);

			for (int i = 0; i < need_thread_num; i++)
			{
				int st_id = per_num*i;
				int end_id = __min(l_count, per_num*(i + 1));
				int cur_num = end_id - st_id;
				if (cur_num > 0)
				{
					task_src_off_x[i].resize(cur_num);
					task_src_off_y[i].resize(cur_num);
					task_src_rect_w[i].resize(cur_num);
					task_src_rect_h[i].resize(cur_num);
					task_fourthBbox[i].resize(cur_num);
					for (int j = 0; j < cur_num; j++)
					{
						task_src_off_x[i][j] = src_off_x[st_id + j];
						task_src_off_y[i][j] = src_off_y[st_id + j];
						task_src_rect_w[i][j] = src_rect_w[st_id + j];
						task_src_rect_h[i][j] = src_rect_h[st_id + j];
						task_fourthBbox[i][j].col1 = copy_fourthBbox[st_id + j].col1;
						task_fourthBbox[i][j].col2 = copy_fourthBbox[st_id + j].col2;
						task_fourthBbox[i][j].row1 = copy_fourthBbox[st_id + j].row1;
						task_fourthBbox[i][j].row2 = copy_fourthBbox[st_id + j].row2;
						task_fourthBbox[i][j].area = copy_fourthBbox[st_id + j].area;
						task_fourthBbox[i][j].score = copy_fourthBbox[st_id + j].score;
						task_fourthBbox[i][j].exist = copy_fourthBbox[st_id + j].exist;
					}
				}
			}

			resultBbox.resize(l_count);
			for (int i = 0; i < l_count; i++)
			{
				resultBbox[i].col1 = fourthBbox[i].col1;
				resultBbox[i].col2 = fourthBbox[i].col2;
				resultBbox[i].row1 = fourthBbox[i].row1;
				resultBbox[i].row2 = fourthBbox[i].row2;
				resultBbox[i].score = fourthBbox[i].score;
				resultBbox[i].exist = fourthBbox[i].exist;
				resultBbox[i].area = fourthBbox[i].area;
			}

			for (int pp = 0; pp < need_thread_num; pp++)
			{
				if (task_src_off_x[pp].size() == 0)
					continue;
				if (!input.ResizeBilinearRect(task_lnet_images[pp], lnet106_size, lnet106_size, 0, 0,
					task_src_off_x[pp], task_src_off_y[pp], task_src_rect_w[pp], task_src_rect_h[pp]))
				{
					continue;
				}
				double t31 = omp_get_wtime();
				lnets106[0].Forward(task_lnet_images[pp]);
				double t32 = omp_get_wtime();
				const ZQ_CNN_Tensor4D* keyPoint = lnets106[0].GetBlobByName("conv6-3");
				const float* keyPoint_ptr = keyPoint->GetFirstPixelPtr();
				int keypoint_num = keyPoint->GetC() / 2;
				int keyPoint_sliceStep = keyPoint->GetSliceStep();
				for (int i = 0; i < task_fourthBbox[pp].size(); i++)
				{
					for (int num = 0; num < keypoint_num; num++)
					{
						task_fourthBbox[pp][i].ppoint[num * 2] = task_fourthBbox[pp][i].col1 +
							(task_fourthBbox[pp][i].col2 - task_fourthBbox[pp][i].col1)*keyPoint_ptr[i*keyPoint_sliceStep + num * 2];
						task_fourthBbox[pp][i].ppoint[num * 2 + 1] = task_fourthBbox[pp][i].row1 +
							(task_fourthBbox[pp][i].row2 - task_fourthBbox[pp][i].row1)*keyPoint_ptr[i*keyPoint_sliceStep + num * 2 + 1];
					}
				}
			}

			int count = 0;
			for (int i = 0; i < need_thread_num; i++)
			{
				count += task_fourthBbox[i].size();
			}
			resultBbox.resize(count);
			int id = 0;
			for (int i = 0; i < need_thread_num; i++)
			{
				for (int j = 0; j < task_fourthBbox[i].size(); j++)
				{
					memcpy(resultBbox[id].ppoint, task_fourthBbox[i][j].ppoint, sizeof(float) * 212);
					id++;
				}
			}
			double t5 = omp_get_wtime();
			if (show_debug_info)
				printf("run Lnet [%d] times \n", l_count);
			if (show_debug_info)
				printf("stage 4: cost %.3f ms\n", 1000 * (t5 - t4));

			return true;
		}

		void _filtering(const std::vector<std::vector<ZQ_CNN_BBox106> >& trace, std::vector<ZQ_CNN_BBox106>& results)
		{
			float reproj_coords[212];
			for (int i = 0; i < results.size(); i++)
			{
				const std::vector<ZQ_CNN_BBox106>& cur_trace = trace[i];
				results[i] = cur_trace[0];
				const ZQ_CNN_BBox106& cur_box = trace[i][0];
				const float reproj_thresh = 0.005f;
				float real_thresh = reproj_thresh*(cur_box.col2 - cur_box.col1 + cur_box.row2 - cur_box.row1);
				float sum_weight = 1.0f;
				int cur_trace_len = cur_trace.size();
				if (cur_trace_len <= 1)
					continue;

				for(int j = 1;j < cur_trace_len;j++)
				{
					double reproj_err = 0;
					float last_weight = exp(-weight_decay*j);
					const ZQ_CNN_BBox106& last_box = trace[i][j];
					_compute_transform(last_box.ppoint, cur_box.ppoint, reproj_err, reproj_coords);
					
					if (reproj_err < real_thresh)
					{
						//printf("reproj_err = %f, real_thresh = %f\n", reproj_err, real_thresh);
						for (int j = 0; j < 212; j++)
						{
							results[i].ppoint[j] += last_box.ppoint[j] * last_weight;
						}
						sum_weight += last_weight;
					}
					else
					{
						//printf("reproj_err = %f\n", reproj_err);
					}
				}

				for (int j = 0; j < 212; j++)
				{
					results[i].ppoint[j] /= sum_weight;
				}
			}
		}

		void _compute_transform(const float last_pts[], const float cur_pts[], double& reproj_err, float reproj_coords[])
		{
			ZQ_Matrix<double> A(212, 6), b(212, 1), x(6,1);
			for (int i = 0; i < 106; i++)
			{
				A.SetData(i * 2, 0, last_pts[i * 2]);
				A.SetData(i * 2, 1, last_pts[i * 2 + 1]);
				A.SetData(i * 2, 4, 1);
				b.SetData(i * 2, 0, cur_pts[i * 2]);
				A.SetData(i * 2 + 1, 2, last_pts[i * 2]);
				A.SetData(i * 2 + 1, 3, last_pts[i * 2 + 1]);
				A.SetData(i * 2 + 1, 5, 1);
				b.SetData(i * 2 + 1, 0, cur_pts[i * 2 + 1]);
			}
			if (!ZQ_SVD::Solve(A, x, b))
			{
				reproj_err = FLT_MAX;
			}
			else
			{
				bool flag;
				float R[4],T[2];
				R[0] = x.GetData(0, 0, flag);
				R[1] = x.GetData(1, 0, flag);
				R[2] = x.GetData(2, 0, flag);
				R[3] = x.GetData(3, 0, flag);
				T[0] = x.GetData(4, 0, flag);
				T[1] = x.GetData(5, 0, flag);
				reproj_err = 0;
				for (int i = 0; i < 106; i++)
				{
					reproj_coords[i * 2 + 0] = last_pts[i * 2 + 0] * R[0] + last_pts[i * 2 + 1] * R[1] + T[0];
					reproj_coords[i * 2 + 1] = last_pts[i * 2 + 0] * R[2] + last_pts[i * 2 + 1] * R[3] + T[1];
					reproj_err += fabs(reproj_coords[i * 2 + 0] - cur_pts[i * 2 + 0]);
					reproj_err += fabs(reproj_coords[i * 2 + 1] - cur_pts[i * 2 + 1]);
				}
				reproj_err /= 212.0;
			}
		}

		void _recompute_bbox(std::vector<ZQ_CNN_BBox106>& boxes)
		{
			for (int i = 0; i < boxes.size(); i++)
			{
				float xmin = FLT_MAX;
				float ymin = FLT_MAX;
				float xmax = -FLT_MAX;
				float ymax = -FLT_MAX;
				float cx, cy,max_side;
				for (int j = 0; j < 106; j++)
				{
					float* coords = boxes[i].ppoint;
					xmin = __min(xmin, coords[j * 2]);
					xmax = __max(xmax, coords[j * 2]);
					ymin = __min(ymin, coords[j * 2 + 1]);
					ymax = __max(ymax, coords[j * 2 + 1]);
				}
				cx = 0.5*(xmin + xmax);
				cy = 0.5*(ymin + ymax);
				max_side = __max((xmax - xmin), (ymax - ymin));
				xmin = round(cx - 0.5*max_side);
				xmax = round(cx + 0.5*max_side);
				ymin = round(cy - 0.5*max_side);
				ymax = round(cy + 0.5*max_side);
				boxes[i].col1 = xmin;
				boxes[i].col2 = xmax;
				boxes[i].row1 = ymin;
				boxes[i].row2 = ymax;
				boxes[i].area = (xmax - xmin)*(ymax - ymin);
			}
		}
	};
}
#endif
