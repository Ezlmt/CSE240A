//========================================================//
//  predictor.c                                           //
//  Source file for the Branch Predictor                  //
//                                                        //
//  Implement the various branch predictors below as      //
//  described in the README                               //
//========================================================//
#include <stdio.h>
#include "predictor.h"

//
// TODO:Student Information
//
const char *studentName = "NAME";
const char *studentID   = "PID";
const char *email       = "EMAIL";

//------------------------------------//
//      Predictor Configuration       //
//------------------------------------//

// Handy Global for use in output routines
const char *bpName[4] = { "Static", "Gshare",
                          "Tournament", "Custom" };

int ghistoryBits; // Number of bits used for Global History
int lhistoryBits; // Number of bits used for Local History
int pcIndexBits;  // Number of bits used for PC index
int bpType;       // Branch Prediction Type
int verbose;

//------------------------------------//
//      Predictor Data Structures     //
//------------------------------------//

// Gshare Predictor Data Structures
uint32_t *gshare_bht;       // Branch History Table
uint32_t gshare_history;    // Global History Register

// Tournament Predictor Data Structures
uint32_t *tournament_global_bht;    // Global Branch History Table
uint32_t *tournament_local_bht;     // Local Branch History Table
uint32_t *tournament_local_history;  // Local History Table
uint32_t *tournament_choice;         // Choice Predictor
uint32_t tournament_global_history;  // Global History Register

// Custom Predictor Data Structures
#define CUSTOM_GHIST_BITS 18    // 增加全局历史位数
#define CUSTOM_PHT_BITS 16      // 模式历史表位数
#define CUSTOM_BHT_BITS 14      // 分支历史表位数
#define CUSTOM_LPT_BITS 12      // 增加循环预测表位数
#define CUSTOM_LPT_TAG_BITS 16  // 循环预测表标签位数
#define CUSTOM_LPT_CONF_BITS 4  // 循环置信度位数
#define CUSTOM_LHIST_BITS 14    // 增加局部历史位数
#define CUSTOM_PC_BITS 12       // 增加PC索引位数
#define CUSTOM_META_BITS 12     // 元预测器位数
#define CUSTOM_SIMPLE_BITS 12   // 简单PC预测器位数
#define CUSTOM_INT_BITS 10      // 整数预测器位数

typedef struct {
    uint32_t tag;           // 循环标签
    uint32_t confidence;    // 置信度计数器
    uint32_t iter_count;    // 迭代计数器
    uint8_t  is_loop;      // 是否为循环分支
    uint32_t last_outcome; // 上次结果
    uint32_t pattern;      // 循环模式
    uint32_t depth;        // 循环嵌套深度
} loop_entry_t;

typedef struct {
    uint32_t global_correct;    // 全局预测器正确次数
    uint32_t local_correct;     // 局部预测器正确次数
    uint32_t loop_correct;      // 循环预测器正确次数
    uint32_t hybrid_correct;    // 混合预测器正确次数
    uint32_t simple_correct;    // 简单预测器正确次数
    uint32_t int_correct;       // 整数预测器正确次数
    uint32_t total_count;       // 总预测次数
    uint32_t recent_window;     // 最近窗口计数
    uint32_t int_branches;      // 整数分支计数
    uint32_t loop_branches;     // 循环分支计数
} meta_stats_t;

uint32_t *custom_pht;           // 模式历史表 (全局)
uint32_t *custom_bht;           // 分支历史表 (混合)
uint32_t *custom_lht;           // 局部历史表
uint32_t *custom_simple;        // 简单PC预测器
uint32_t *custom_int;           // 整数专用预测器
uint32_t *custom_local_history; // 局部历史寄存器
uint32_t *custom_meta;          // 元预测器表
uint32_t custom_history;        // 全局历史寄存器
uint32_t custom_path_history;   // 路径历史
loop_entry_t *custom_lpt;       // 循环预测表
meta_stats_t custom_stats;      // 全局统计信息

// 存储大小计算 (bits):
// custom_pht: 2^16 * 2 = 131,072
// custom_bht: 2^14 * 2 = 32,768
// custom_lht: 2^12 * 2 = 8,192
// custom_simple: 2^12 * 2 = 8,192
// custom_local_history: 2^10 * 12 = 12,288
// custom_meta: 2^12 * 3 = 12,288
// custom_lpt: 2^10 * 96 = 98,304
// 历史寄存器: 16
// 总计: 约305KB

// Helper functions for bit manipulation
#define MASK(bits) ((1 << (bits)) - 1)

// Helper functions for 2-bit counter
uint8_t get_prediction_from_counter(uint8_t counter) {
    return (counter >= WT) ? TAKEN : NOTTAKEN;
}

uint8_t update_counter(uint8_t counter, uint8_t outcome) {
    if (outcome == TAKEN) {
        return (counter == ST) ? ST : counter + 1;
    } else {
        return (counter == SN) ? SN : counter - 1;
    }
}

// 计算哈希索引的辅助函数
uint32_t compute_hash_1(uint32_t pc, uint32_t history) {
    return ((pc >> 2) ^ history ^ (history >> 4) ^ (pc >> 10)) & MASK(CUSTOM_PHT_BITS);
}

uint32_t compute_hash_2(uint32_t pc, uint32_t history) {
    return ((pc >> 3) ^ (history << 2) ^ (history >> 2) ^ custom_path_history) & MASK(CUSTOM_BHT_BITS);
}

uint32_t compute_hash_3(uint32_t pc, uint32_t history) {
    return ((pc >> 4) ^ history ^ (pc >> 8) ^ (history << 3)) & MASK(CUSTOM_LHIST_BITS);
}

// 检测是否为整数分支
uint8_t is_int_branch(uint32_t pc) {
    // 使用PC的特征来判断
    uint32_t pc_pattern = (pc >> 2) & 0xFF;
    return (pc_pattern < 0x80); // 简单启发式判断
}

// 估计循环嵌套深度
uint32_t estimate_loop_depth(uint32_t pc, uint32_t history) {
    uint32_t depth = 0;
    uint32_t pattern = history;
    while (pattern) {
        if (pattern & 1) depth++;
        pattern >>= 1;
    }
    return (depth > 3) ? 3 : depth;
}

//------------------------------------//
//        Predictor Functions         //
//------------------------------------//

// Initialize the predictor
//
void
init_predictor()
{
    // Initialize Gshare
    if (bpType == GSHARE) {
        // Allocate BHT - size is 2^ghistoryBits entries
        gshare_bht = (uint32_t *)calloc(1 << ghistoryBits, sizeof(uint32_t));
        // Initialize all entries to WN (1)
        for (int i = 0; i < (1 << ghistoryBits); i++) {
            gshare_bht[i] = WN;
        }
        // Initialize global history to NOTTAKEN (0)
        gshare_history = 0;
    }
    
    // Initialize Tournament
    else if (bpType == TOURNAMENT) {
        // Allocate and initialize global BHT
        tournament_global_bht = (uint32_t *)calloc(1 << ghistoryBits, sizeof(uint32_t));
        for (int i = 0; i < (1 << ghistoryBits); i++) {
            tournament_global_bht[i] = WN;
        }
        
        // Allocate and initialize local history table
        tournament_local_history = (uint32_t *)calloc(1 << pcIndexBits, sizeof(uint32_t));
        // History initialized to 0 (NOTTAKEN) by calloc
        
        // Allocate and initialize local BHT
        tournament_local_bht = (uint32_t *)calloc(1 << lhistoryBits, sizeof(uint32_t));
        for (int i = 0; i < (1 << lhistoryBits); i++) {
            tournament_local_bht[i] = WN;
        }
        
        // Allocate and initialize choice predictor
        tournament_choice = (uint32_t *)calloc(1 << ghistoryBits, sizeof(uint32_t));
        for (int i = 0; i < (1 << ghistoryBits); i++) {
            tournament_choice[i] = WN;  // Weakly favor Global
        }
        
        // Initialize global history to NOTTAKEN (0)
        tournament_global_history = 0;
    }
    
    // Initialize Custom
    else if (bpType == CUSTOM) {
        // 分配并初始化全局模式历史表
        custom_pht = (uint32_t *)calloc(1 << CUSTOM_PHT_BITS, sizeof(uint32_t));
        for (int i = 0; i < (1 << CUSTOM_PHT_BITS); i++) {
            custom_pht[i] = WN;
        }
        
        // 分配并初始化混合分支历史表
        custom_bht = (uint32_t *)calloc(1 << CUSTOM_BHT_BITS, sizeof(uint32_t));
        for (int i = 0; i < (1 << CUSTOM_BHT_BITS); i++) {
            custom_bht[i] = WN;
        }
        
        // 分配并初始化局部历史表
        custom_lht = (uint32_t *)calloc(1 << CUSTOM_LHIST_BITS, sizeof(uint32_t));
        for (int i = 0; i < (1 << CUSTOM_LHIST_BITS); i++) {
            custom_lht[i] = WN;
        }
        
        // 分配并初始化简单PC预测器
        custom_simple = (uint32_t *)calloc(1 << CUSTOM_SIMPLE_BITS, sizeof(uint32_t));
        for (int i = 0; i < (1 << CUSTOM_SIMPLE_BITS); i++) {
            custom_simple[i] = WN;
        }
        
        // 分配并初始化局部历史寄存器
        custom_local_history = (uint32_t *)calloc(1 << CUSTOM_PC_BITS, sizeof(uint32_t));
        
        // 分配并初始化元预测器
        custom_meta = (uint32_t *)calloc(1 << CUSTOM_META_BITS, sizeof(uint32_t));
        for (int i = 0; i < (1 << CUSTOM_META_BITS); i++) {
            custom_meta[i] = 1;  // 初始偏向全局预测器
        }
        
        // 分配并初始化循环预测表
        custom_lpt = (loop_entry_t *)calloc(1 << CUSTOM_LPT_BITS, sizeof(loop_entry_t));
        
        // 初始化全局历史寄存器和统计信息
        custom_history = 0;
        custom_stats.global_correct = 0;
        custom_stats.local_correct = 0;
        custom_stats.loop_correct = 0;
        custom_stats.hybrid_correct = 0;
        custom_stats.simple_correct = 0;
        custom_stats.total_count = 0;
        custom_stats.recent_window = 0;
    }
}

// Make a prediction for conditional branch instruction at PC 'pc'
// Returning TAKEN indicates a prediction of taken; returning NOTTAKEN
// indicates a prediction of not taken
//
uint8_t
make_prediction(uint32_t pc)
{
  // Make a prediction based on the bpType
  switch (bpType) {
    case STATIC:
      return TAKEN;
            
        case GSHARE: {
            // XOR PC with global history
            uint32_t index = ((pc >> 2) ^ gshare_history) & MASK(ghistoryBits);
            // Get prediction from BHT
            return get_prediction_from_counter(gshare_bht[index]);
        }
            
        case TOURNAMENT: {
            // Get local history index using PC
            uint32_t local_history_index = (pc >> 2) & MASK(pcIndexBits);
            uint32_t local_history = tournament_local_history[local_history_index];
            
            // Get predictions from both predictors
            uint32_t local_bht_index = local_history & MASK(lhistoryBits);
            uint32_t global_bht_index = tournament_global_history & MASK(ghistoryBits);
            
            uint8_t local_pred = get_prediction_from_counter(tournament_local_bht[local_bht_index]);
            uint8_t global_pred = get_prediction_from_counter(tournament_global_bht[global_bht_index]);
            
            // Use choice predictor to select between local and global
            uint32_t choice_index = tournament_global_history & MASK(ghistoryBits);
            uint8_t choice = get_prediction_from_counter(tournament_choice[choice_index]);
            
            return (choice == TAKEN) ? global_pred : local_pred;
        }
            
        case CUSTOM: {
            // 计算各种哈希索引
            uint32_t pc_index = (pc >> 2) & MASK(CUSTOM_PC_BITS);
            uint32_t local_history = custom_local_history[pc_index];
            
            // 全局预测器索引
            uint32_t global_index = compute_hash_1(pc, custom_history) & MASK(CUSTOM_PHT_BITS);
            
            // 混合预测器索引
            uint32_t hybrid_index = compute_hash_2(pc, custom_history) & MASK(CUSTOM_BHT_BITS);
            
            // 局部预测器索引
            uint32_t local_index = compute_hash_3(pc, local_history) & MASK(CUSTOM_LHIST_BITS);
            
            // 简单PC预测器索引
            uint32_t simple_index = (pc >> 3) & MASK(CUSTOM_SIMPLE_BITS);
            
            // 整数预测器索引
            uint32_t int_index = ((pc >> 2) ^ (pc >> 8)) & MASK(CUSTOM_INT_BITS);
            
            // 循环预测器索引
            uint32_t loop_index = ((pc >> 4) ^ (pc >> 8)) & MASK(CUSTOM_LPT_BITS);
            uint32_t loop_tag = (pc >> 2) & MASK(CUSTOM_LPT_TAG_BITS);
            
            // 元预测器索引
            uint32_t meta_index = ((pc >> 2) ^ custom_history ^ custom_path_history) & MASK(CUSTOM_META_BITS);
            
            // 获取预测结果用于统计
            uint8_t global_pred = get_prediction_from_counter(custom_pht[global_index]);
            uint8_t hybrid_pred = get_prediction_from_counter(custom_bht[hybrid_index]);
            uint8_t local_pred = get_prediction_from_counter(custom_lht[local_index]);
            uint8_t simple_pred = get_prediction_from_counter(custom_simple[simple_index]);
            uint8_t int_pred = get_prediction_from_counter(custom_int[int_index]);
            
            // 检查是否为整数分支
            uint8_t is_int = is_int_branch(pc);
            if (is_int) {
                custom_stats.int_branches++;
            }
            
            // 更新统计信息
            custom_stats.total_count++;
            custom_stats.recent_window++;
            
            if (global_pred == outcome) custom_stats.global_correct++;
            if (local_pred == outcome) custom_stats.local_correct++;
            if (hybrid_pred == outcome) custom_stats.hybrid_correct++;
            if (simple_pred == outcome) custom_stats.simple_correct++;
            if (int_pred == outcome) custom_stats.int_correct++;
            
            // 每10000次预测重置最近窗口统计，保持自适应性
            if (custom_stats.recent_window >= 10000) {
                custom_stats.global_correct = (custom_stats.global_correct * 8) / 10;
                custom_stats.local_correct = (custom_stats.local_correct * 8) / 10;
                custom_stats.hybrid_correct = (custom_stats.hybrid_correct * 8) / 10;
                custom_stats.simple_correct = (custom_stats.simple_correct * 8) / 10;
                custom_stats.int_correct = (custom_stats.int_correct * 8) / 10;
                custom_stats.loop_correct = (custom_stats.loop_correct * 8) / 10;
                custom_stats.total_count = (custom_stats.total_count * 8) / 10;
                custom_stats.int_branches = (custom_stats.int_branches * 8) / 10;
                custom_stats.loop_branches = (custom_stats.loop_branches * 8) / 10;
                custom_stats.recent_window = 0;
            }
            
            // 更新循环预测器
            loop_entry_t *loop_entry = &custom_lpt[loop_index];
            if (loop_entry->tag == loop_tag) {
                // 已知分支
                if (outcome == TAKEN) {
                    loop_entry->iter_count++;
                    custom_stats.loop_branches++;
                    
                    // 更新循环模式
                    loop_entry->pattern = ((loop_entry->pattern << 1) | 1) & 0xFFFF;
                    
                    if (loop_entry->iter_count >= 3) {
                        loop_entry->is_loop = 1;
                        if (loop_entry->confidence < ((1 << CUSTOM_LPT_CONF_BITS) - 1)) {
                            loop_entry->confidence++;
                        }
                    }
                    
                    // 更新循环嵌套深度
                    uint32_t depth = estimate_loop_depth(pc, custom_history);
                    loop_entry->depth = depth;
                } else {
                    // 分支未taken，可能是循环结束
                    if (loop_entry->is_loop && loop_entry->iter_count > 0) {
                        // 预测循环结束
                        uint8_t loop_pred;
                        if (loop_entry->pattern & 1) {
                            loop_pred = TAKEN;
                        } else {
                            loop_pred = ((loop_entry->iter_count + loop_entry->depth) % (8 >> loop_entry->depth) == 0) ? NOTTAKEN : TAKEN;
                        }
                        
                        if (loop_pred == outcome) {
                            custom_stats.loop_correct++;
                            if (loop_entry->confidence < ((1 << CUSTOM_LPT_CONF_BITS) - 1)) {
                                loop_entry->confidence++;
                            }
                        } else {
                            if (loop_entry->confidence > 0) {
                                loop_entry->confidence--;
                            }
                        }
                    }
                    
                    // 更新模式
                    loop_entry->pattern = (loop_entry->pattern << 1) & 0xFFFF;
                    loop_entry->iter_count = 0;
                    
                    if (loop_entry->confidence <= 1) {
                        loop_entry->is_loop = 0;
                    }
                }
                loop_entry->last_outcome = outcome;
            } else {
                // 新分支
                loop_entry->tag = loop_tag;
                loop_entry->confidence = 0;
                loop_entry->iter_count = (outcome == TAKEN) ? 1 : 0;
                loop_entry->is_loop = 0;
                loop_entry->last_outcome = outcome;
                loop_entry->pattern = outcome ? 1 : 0;
                loop_entry->depth = estimate_loop_depth(pc, custom_history);
            }
            
            // 更新元预测器
            uint8_t best_predictor = 0;  // 0: local, 1: global, 2: hybrid, 3: simple
            uint8_t correct_count = 0;
            
            if (global_pred == outcome) correct_count++;
            if (local_pred == outcome) correct_count++;
            if (hybrid_pred == outcome) correct_count++;
            if (simple_pred == outcome) correct_count++;
            if (int_pred == outcome) correct_count++;
            
            // 选择最佳预测器
            if (correct_count == 1) {
                // 只有一个预测器正确
                if (global_pred == outcome) best_predictor = 1;
                else if (local_pred == outcome) best_predictor = 0;
                else if (hybrid_pred == outcome) best_predictor = 2;
                else if (simple_pred == outcome) best_predictor = 3;
            } else if (correct_count > 1) {
                // 多个预测器正确，根据当前权重选择
                uint32_t total = custom_stats.total_count;
                if (total > 0) {
                    uint32_t global_weight = (custom_stats.global_correct * 100) / total;
                    uint32_t local_weight = (custom_stats.local_correct * 100) / total;
                    uint32_t hybrid_weight = (custom_stats.hybrid_correct * 100) / total;
                    uint32_t simple_weight = (custom_stats.simple_correct * 100) / total;
                    uint32_t int_weight = (custom_stats.int_correct * 100) / total;
                    
                    if (is_int && int_weight >= 45) {
                        best_predictor = 0;  // 整数分支偏向局部预测
                    } else if (global_weight >= local_weight && global_weight >= hybrid_weight && global_weight >= simple_weight) {
                        best_predictor = 1;
                    } else if (hybrid_weight >= local_weight && hybrid_weight >= simple_weight) {
                        best_predictor = 2;
                    } else if (simple_weight >= local_weight) {
                        best_predictor = 3;
                    } else {
                        best_predictor = 0;
                    }
                }
            }
            
            // 温和更新元预测器
            if (best_predictor == 1 && custom_meta[meta_index] < 3) {
                custom_meta[meta_index]++;
            } else if (best_predictor == 0 && custom_meta[meta_index] > 0) {
                custom_meta[meta_index]--;
            }
            
            // 更新各个预测器
            custom_pht[global_index] = update_counter(custom_pht[global_index], outcome);
            custom_bht[hybrid_index] = update_counter(custom_bht[hybrid_index], outcome);
            custom_lht[local_index] = update_counter(custom_lht[local_index], outcome);
            custom_simple[simple_index] = update_counter(custom_simple[simple_index], outcome);
            custom_int[int_index] = update_counter(custom_int[int_index], outcome);
            
            // 更新历史寄存器
            custom_local_history[pc_index] = ((local_history << 1) | outcome) & MASK(CUSTOM_LHIST_BITS);
            custom_history = ((custom_history << 1) | outcome) & MASK(CUSTOM_GHIST_BITS);
            custom_path_history = ((custom_path_history << 1) | (pc & 1)) & MASK(CUSTOM_GHIST_BITS);
            break;
        }
            
    default:
      break;
  }

    // If there is not a compatible bpType then return NOTTAKEN
  return NOTTAKEN;
}

// Train the predictor
void
train_predictor(uint32_t pc, uint8_t outcome)
{
    switch (bpType) {
        case STATIC:
            // Static predictor is not trained
            break;
            
        case GSHARE: {
            // Get index into BHT
            uint32_t index = ((pc >> 2) ^ gshare_history) & MASK(ghistoryBits);
            
            // Update counter
            gshare_bht[index] = update_counter(gshare_bht[index], outcome);
            
            // Update global history register
            gshare_history = ((gshare_history << 1) | outcome) & MASK(ghistoryBits);
            break;
        }
            
        case TOURNAMENT: {
            // Get local history index using PC
            uint32_t local_history_index = (pc >> 2) & MASK(pcIndexBits);
            uint32_t local_history = tournament_local_history[local_history_index];
            
            // Get predictions from both predictors
            uint32_t local_bht_index = local_history & MASK(lhistoryBits);
            uint32_t global_bht_index = tournament_global_history & MASK(ghistoryBits);
            
            uint8_t local_pred = get_prediction_from_counter(tournament_local_bht[local_bht_index]);
            uint8_t global_pred = get_prediction_from_counter(tournament_global_bht[global_bht_index]);
            
            // Update choice predictor
            uint32_t choice_index = tournament_global_history & MASK(ghistoryBits);
            if (local_pred != global_pred) {
                if (local_pred == outcome) {
                    // Local prediction was correct, train choice predictor to prefer local
                    tournament_choice[choice_index] = update_counter(tournament_choice[choice_index], NOTTAKEN);
                } else {
                    // Global prediction was correct, train choice predictor to prefer global
                    tournament_choice[choice_index] = update_counter(tournament_choice[choice_index], TAKEN);
                }
            }
            
            // Update local predictor
            tournament_local_bht[local_bht_index] = update_counter(tournament_local_bht[local_bht_index], outcome);
            
            // Update global predictor
            tournament_global_bht[global_bht_index] = update_counter(tournament_global_bht[global_bht_index], outcome);
            
            // Update history registers
            tournament_local_history[local_history_index] = ((local_history << 1) | outcome) & MASK(lhistoryBits);
            tournament_global_history = ((tournament_global_history << 1) | outcome) & MASK(ghistoryBits);
            break;
        }
            
        case CUSTOM: {
            // 计算各种哈希索引
            uint32_t pc_index = (pc >> 2) & MASK(CUSTOM_PC_BITS);
            uint32_t local_history = custom_local_history[pc_index];
            
            // 全局预测器索引
            uint32_t global_index = compute_hash_1(pc, custom_history) & MASK(CUSTOM_PHT_BITS);
            
            // 混合预测器索引
            uint32_t hybrid_index = compute_hash_2(pc, custom_history) & MASK(CUSTOM_BHT_BITS);
            
            // 局部预测器索引
            uint32_t local_index = compute_hash_3(pc, local_history) & MASK(CUSTOM_LHIST_BITS);
            
            // 简单PC预测器索引
            uint32_t simple_index = (pc >> 3) & MASK(CUSTOM_SIMPLE_BITS);
            
            // 整数预测器索引
            uint32_t int_index = ((pc >> 2) ^ (pc >> 8)) & MASK(CUSTOM_INT_BITS);
            
            // 循环预测器索引
            uint32_t loop_index = ((pc >> 4) ^ (pc >> 8)) & MASK(CUSTOM_LPT_BITS);
            uint32_t loop_tag = (pc >> 2) & MASK(CUSTOM_LPT_TAG_BITS);
            
            // 元预测器索引
            uint32_t meta_index = ((pc >> 2) ^ custom_history ^ custom_path_history) & MASK(CUSTOM_META_BITS);
            
            // 获取预测结果用于统计
            uint8_t global_pred = get_prediction_from_counter(custom_pht[global_index]);
            uint8_t hybrid_pred = get_prediction_from_counter(custom_bht[hybrid_index]);
            uint8_t local_pred = get_prediction_from_counter(custom_lht[local_index]);
            uint8_t simple_pred = get_prediction_from_counter(custom_simple[simple_index]);
            uint8_t int_pred = get_prediction_from_counter(custom_int[int_index]);
            
            // 检查是否为整数分支
            uint8_t is_int = is_int_branch(pc);
            if (is_int) {
                custom_stats.int_branches++;
            }
            
            // 更新统计信息
            custom_stats.total_count++;
            custom_stats.recent_window++;
            
            if (global_pred == outcome) custom_stats.global_correct++;
            if (local_pred == outcome) custom_stats.local_correct++;
            if (hybrid_pred == outcome) custom_stats.hybrid_correct++;
            if (simple_pred == outcome) custom_stats.simple_correct++;
            if (int_pred == outcome) custom_stats.int_correct++;
            
            // 每10000次预测重置最近窗口统计，保持自适应性
            if (custom_stats.recent_window >= 10000) {
                custom_stats.global_correct = (custom_stats.global_correct * 8) / 10;
                custom_stats.local_correct = (custom_stats.local_correct * 8) / 10;
                custom_stats.hybrid_correct = (custom_stats.hybrid_correct * 8) / 10;
                custom_stats.simple_correct = (custom_stats.simple_correct * 8) / 10;
                custom_stats.int_correct = (custom_stats.int_correct * 8) / 10;
                custom_stats.loop_correct = (custom_stats.loop_correct * 8) / 10;
                custom_stats.total_count = (custom_stats.total_count * 8) / 10;
                custom_stats.int_branches = (custom_stats.int_branches * 8) / 10;
                custom_stats.loop_branches = (custom_stats.loop_branches * 8) / 10;
                custom_stats.recent_window = 0;
            }
            
            // 更新循环预测器
            loop_entry_t *loop_entry = &custom_lpt[loop_index];
            if (loop_entry->tag == loop_tag) {
                // 已知分支
                if (outcome == TAKEN) {
                    loop_entry->iter_count++;
                    custom_stats.loop_branches++;
                    
                    // 更新循环模式
                    loop_entry->pattern = ((loop_entry->pattern << 1) | 1) & 0xFFFF;
                    
                    if (loop_entry->iter_count >= 3) {
                        loop_entry->is_loop = 1;
                        if (loop_entry->confidence < ((1 << CUSTOM_LPT_CONF_BITS) - 1)) {
                            loop_entry->confidence++;
                        }
                    }
                    
                    // 更新循环嵌套深度
                    uint32_t depth = estimate_loop_depth(pc, custom_history);
                    loop_entry->depth = depth;
                } else {
                    // 分支未taken，可能是循环结束
                    if (loop_entry->is_loop && loop_entry->iter_count > 0) {
                        // 预测循环结束
                        uint8_t loop_pred;
                        if (loop_entry->pattern & 1) {
                            loop_pred = TAKEN;
                        } else {
                            loop_pred = ((loop_entry->iter_count + loop_entry->depth) % (8 >> loop_entry->depth) == 0) ? NOTTAKEN : TAKEN;
                        }
                        
                        if (loop_pred == outcome) {
                            custom_stats.loop_correct++;
                            if (loop_entry->confidence < ((1 << CUSTOM_LPT_CONF_BITS) - 1)) {
                                loop_entry->confidence++;
                            }
                        } else {
                            if (loop_entry->confidence > 0) {
                                loop_entry->confidence--;
                            }
                        }
                    }
                    
                    // 更新模式
                    loop_entry->pattern = (loop_entry->pattern << 1) & 0xFFFF;
                    loop_entry->iter_count = 0;
                    
                    if (loop_entry->confidence <= 1) {
                        loop_entry->is_loop = 0;
                    }
                }
                loop_entry->last_outcome = outcome;
            } else {
                // 新分支
                loop_entry->tag = loop_tag;
                loop_entry->confidence = 0;
                loop_entry->iter_count = (outcome == TAKEN) ? 1 : 0;
                loop_entry->is_loop = 0;
                loop_entry->last_outcome = outcome;
                loop_entry->pattern = outcome ? 1 : 0;
                loop_entry->depth = estimate_loop_depth(pc, custom_history);
            }
            
            // 更新元预测器
            uint8_t best_predictor = 0;  // 0: local, 1: global, 2: hybrid, 3: simple
            uint8_t correct_count = 0;
            
            if (global_pred == outcome) correct_count++;
            if (local_pred == outcome) correct_count++;
            if (hybrid_pred == outcome) correct_count++;
            if (simple_pred == outcome) correct_count++;
            if (int_pred == outcome) correct_count++;
            
            // 选择最佳预测器
            if (correct_count == 1) {
                // 只有一个预测器正确
                if (global_pred == outcome) best_predictor = 1;
                else if (local_pred == outcome) best_predictor = 0;
                else if (hybrid_pred == outcome) best_predictor = 2;
                else if (simple_pred == outcome) best_predictor = 3;
            } else if (correct_count > 1) {
                // 多个预测器正确，根据当前权重选择
                uint32_t total = custom_stats.total_count;
                if (total > 0) {
                    uint32_t global_weight = (custom_stats.global_correct * 100) / total;
                    uint32_t local_weight = (custom_stats.local_correct * 100) / total;
                    uint32_t hybrid_weight = (custom_stats.hybrid_correct * 100) / total;
                    uint32_t simple_weight = (custom_stats.simple_correct * 100) / total;
                    uint32_t int_weight = (custom_stats.int_correct * 100) / total;
                    
                    if (is_int && int_weight >= 45) {
                        best_predictor = 0;  // 整数分支偏向局部预测
                    } else if (global_weight >= local_weight && global_weight >= hybrid_weight && global_weight >= simple_weight) {
                        best_predictor = 1;
                    } else if (hybrid_weight >= local_weight && hybrid_weight >= simple_weight) {
                        best_predictor = 2;
                    } else if (simple_weight >= local_weight) {
                        best_predictor = 3;
                    } else {
                        best_predictor = 0;
                    }
                }
            }
            
            // 温和更新元预测器
            if (best_predictor == 1 && custom_meta[meta_index] < 3) {
                custom_meta[meta_index]++;
            } else if (best_predictor == 0 && custom_meta[meta_index] > 0) {
                custom_meta[meta_index]--;
            }
            
            // 更新各个预测器
            custom_pht[global_index] = update_counter(custom_pht[global_index], outcome);
            custom_bht[hybrid_index] = update_counter(custom_bht[hybrid_index], outcome);
            custom_lht[local_index] = update_counter(custom_lht[local_index], outcome);
            custom_simple[simple_index] = update_counter(custom_simple[simple_index], outcome);
            custom_int[int_index] = update_counter(custom_int[int_index], outcome);
            
            // 更新历史寄存器
            custom_local_history[pc_index] = ((local_history << 1) | outcome) & MASK(CUSTOM_LHIST_BITS);
            custom_history = ((custom_history << 1) | outcome) & MASK(CUSTOM_GHIST_BITS);
            custom_path_history = ((custom_path_history << 1) | (pc & 1)) & MASK(CUSTOM_GHIST_BITS);
            break;
        }
            
        default:
            break;
    }
}
