#include "CreditShoppingTask.h"

#include <algorithm>
#include <vector>

#include "Config/TaskData.h"
#include "Controller/Controller.h"
#include "Task/ProcessTask.h"
#include "Utils/Logger.hpp"
#include "Vision/Matcher.h"
#include "Vision/Miscellaneous/CreditShopImageAnalyzer.h"
#include "Vision/OCRer.h"

void asst::CreditShoppingTask::set_black_list(std::vector<std::string> black_list)
{
    m_shopping_list = std::move(black_list);
    m_is_white_list = false;
}

void asst::CreditShoppingTask::set_white_list(std::vector<std::string> black_list)
{
    m_shopping_list = std::move(black_list);
    m_is_white_list = true;
}

asst::CreditShoppingTask& asst::CreditShoppingTask::set_force_shopping_if_credit_full(
    bool force_shopping_if_credit_full) noexcept
{
    m_force_shopping_if_credit_full = force_shopping_if_credit_full;
    return *this;
}

asst::CreditShoppingTask& asst::CreditShoppingTask::set_only_buy_discount(bool only_buy_discount) noexcept
{
    m_only_buy_discount = only_buy_discount;
    return *this;
}

asst::CreditShoppingTask& asst::CreditShoppingTask::set_info_credit_full(bool info_credit_full) noexcept
{
    m_info_credit_full = info_credit_full;
    return *this;
}

asst::CreditShoppingTask& asst::CreditShoppingTask::set_reserve_max_credit(
    bool reserve_max_credit) noexcept
{
    m_reserve_max_credit = reserve_max_credit;
    return *this;
}

int asst::CreditShoppingTask::credit_ocr()
{
    cv::Mat credit_image = ctrler()->get_image();
    OCRer credit_analyzer(credit_image);
    credit_analyzer.set_task_info("CreditShop-CreditOcr");
    credit_analyzer.set_replace(Task.get<OcrTaskInfo>("NumberOcrReplace")->replace_map);

    if (!credit_analyzer.analyze()) {
        Log.trace("ERROR:!credit_analyzer.analyze():");
        return -1;
    }

    const std::string& credit = credit_analyzer.get_result().front().text;

    if (credit.empty() || !ranges::all_of(credit, [](char c) -> bool { return std::isdigit(c); })) {
        return -1;
    }

    Log.trace("credit:", credit);

    return std::stoi(credit);
}

int asst::CreditShoppingTask::discount_ocr(const asst::Rect& commodity)
{
    const auto discount_ocr_task_ptr = Task.get<OcrTaskInfo>("CreditShop-DiscountOcr");
    Rect discount_roi = discount_ocr_task_ptr->roi;

    int x_pos = 0, y_pos = 0;

    if (commodity.x < 200) x_pos = 0;
    else if (commodity.x < 455) x_pos = 1;
    else if (commodity.x < 710) x_pos = 2;
    else if (commodity.x < 965) x_pos = 3;
    else x_pos = 4;

    if (commodity.y < 400) y_pos = 0;
    else y_pos = 1;

    discount_roi.x += Discount_Roi_X[x_pos];
    discount_roi.y += Discount_Roi_Y[y_pos];

    cv::Mat discount_image = ctrler()->get_image();
    OCRer discount_analyzer(discount_image);
    discount_analyzer.set_task_info("CreditShop-DiscountOcr");
    discount_analyzer.set_roi(discount_roi);

    if (!discount_analyzer.analyze()) return 0;

    std::string discount = discount_analyzer.get_result().front().text;

    Log.trace("discount:", discount);

    if (discount.size()<=1||discount.size()>=5) return 0;

    if (discount.front() == '-') discount = discount.substr(1, discount.size() - 1);

    if (discount.back() == '%') discount.pop_back();

    if (discount.empty() || !ranges::all_of(discount, [](char c) -> bool { return std::isdigit(c); })) return 0;

    return std::stoi(discount);
}

bool asst::CreditShoppingTask::credit_shopping(bool white_list_enabled, bool credit_ocr_enabled)
{
    const cv::Mat& image = ctrler()->get_image();

    CreditShopImageAnalyzer shop_analyzer(image);
    if (white_list_enabled) {
        if (m_is_white_list) {
            shop_analyzer.set_white_list(m_shopping_list);
        }
        else {
            shop_analyzer.set_black_list(m_shopping_list);
        }
    }

    if (!shop_analyzer.analyze()) {
        return false;
    }
    const auto& shopping_list = shop_analyzer.get_result();

    for (const Rect& commodity : shopping_list) {
        if (need_exit()) {
            return false;
        }
        if (!m_is_white_list&&m_reserve_max_credit) {
            int credit = credit_ocr();
            if (credit <= MaxCredit) break;
        }

        if (!m_is_white_list && m_only_buy_discount) {
            int discount = discount_ocr(commodity);
            if (discount <= 0) {
                int credit = credit_ocr();
                if (credit > MaxCredit && m_info_credit_full) {
                    json::value cb_info = basic_info();
                    cb_info["what"] = "CreditFullOnlyBuyDiscount";
                    cb_info["details"] = json::object {
                        { "credit", credit },
                    };
                    callback(AsstMsg::SubTaskExtraInfo, cb_info);
                }
                break;
            }
        }

        ctrler()->click(commodity);

        ProcessTask(*this, { "CreditShop-BuyIt" }).run();

        if (ProcessTask(*this, { "CreditShop-NoMoney" }).set_task_delay(0).set_retry_times(0).run()) {
            break;
        }
        if (need_exit()) {
            return false;
        }
        if (credit_ocr_enabled) {
            int credit = credit_ocr();
            if (credit <= MaxCredit) { // 信用值不再溢出，停止购物
                break;
            }
        }
    }

    return true;
}

bool asst::CreditShoppingTask::_run()
{
    Log.trace("CreditShopping: m_is_white_list:", m_is_white_list,
              " m_force_shopping_if_credit_full: ", m_force_shopping_if_credit_full);

    if (!m_force_shopping_if_credit_full) {
        return credit_shopping(true, false);
    }
    else {
        int credit = credit_ocr(); // 识别信用值，防止信用值溢出

        if (credit > MaxCredit) { // 信用值溢出
            return credit_shopping(false, true);
        }
    }
    return true;
}
