#include "twap.h"

#include <opentrade/logger.h>

namespace opentrade {

Instrument* TWAP::Subscribe() {
  return Algo::Subscribe(*st_.sec, st_.src, false);
}

std::string TWAP::OnStart(const ParamMap& params) noexcept {
  st_ = GetParam(params, "Security", st_);
  auto sec = st_.sec;
  assert(sec);  // SecurityTuple already verified before onStart
  assert(st_.acc);
  assert(st_.side);
  assert(st_.qty > 0);
  not_lower_than_last_px_ = st_.position_effect == kOpenPosition &&
                            !IsBuy(st_.side) && kCN == sec->exchange->country &&
                            kStock == sec->type;

  inst_ = Subscribe();
  initial_volume_ = inst_->md().trade.volume;
  auto seconds = GetParam(params, "ValidSeconds", 0);
  if (seconds < 60) return "Too short ValidSeconds, must be >= 60";
  begin_time_ = GetTime();
  end_time_ = begin_time_ + seconds;
  auto err = Modify(params);
  if (!err.empty()) return err;
  if (min_size_ <= 0 && sec->lot_size <= 0) {
    return "MinSize required for security without lot size";
  }
  if (GetParam(params, "InternalCross", kEmptyStr) == "Yes") {
    Cross(st_.qty, price_, st_.side, st_.acc, inst_);
  }
  Timer();
  LOG_DEBUG('[' << name() << ' ' << id() << "] started");
  return {};
}

std::string TWAP::Modify(const ParamMap& params) {
  bool has_value;
  price_ = GetParam(params, "Price", price_, &has_value);
  if (has_value) {
    if (price_ > 0) price_ = RoundPrice(price_);
  }
  min_size_ = GetParam(params, "MinSize", min_size_, &has_value);
  if (has_value) {
    if (min_size_ > 0 && st_.sec->lot_size > 0)
      min_size_ = std::round(min_size_ / st_.sec->lot_size) * st_.sec->lot_size;
  }
  max_floor_ = GetParam(params, "MaxFloor", max_floor_, &has_value);
  if (has_value) {
    if (min_size_ > 0 && max_floor_ < min_size_) max_floor_ = 0;
  }
  max_pov_ = GetParam(params, "MaxPov", max_pov_);
  if (max_pov_ > 1) max_pov_ = 1;
  auto agg = GetParam(params, "Aggression", kEmptyStr, &has_value);
  if (has_value) {
    if (agg == "Low")
      agg_ = kAggLow;
    else if (agg == "Medium")
      agg_ = kAggMedium;
    else if (agg == "High")
      agg_ = kAggHigh;
    else if (agg == "Highest")
      agg_ = kAggHighest;
    else
      return "Invalid aggression, must be in (Low, Medium, High, Highest)";
  }
  return {};
}

void TWAP::OnModify(const ParamMap& params) noexcept {
  auto err = Modify(params);
  if (!err.empty()) {
    LOG_ERROR('[' << name() << ' ' << id() << "] " << err);
  }
}

void TWAP::OnStop() noexcept {
  LOG_DEBUG('[' << name() << ' ' << id() << "] stopped");
}

void TWAP::OnMarketTrade(const Instrument& inst, const MarketData& md,
                         const MarketData& md0) noexcept {
  auto& t = md.trade;
  LOG_DEBUG(inst.sec().symbol << " trade: " << t.open << ' ' << t.high << ' '
                              << t.low << ' ' << t.close << ' ' << t.qty << ' '
                              << t.vwap << ' ' << t.volume);
}

void TWAP::OnMarketQuote(const Instrument& inst, const MarketData& md,
                         const MarketData& md0) noexcept {
  auto& q = md.quote();
  LOG_DEBUG(inst.sec().symbol << " quote: " << q.ask_price << ' ' << q.ask_size
                              << ' ' << q.bid_price << ' ' << q.bid_size);
}

void TWAP::OnConfirmation(const Confirmation& cm) noexcept {
  if (inst_->total_qty() >= st_.qty) Stop();
}

const ParamDefs& TWAP::GetParamDefs() noexcept {
  static ParamDefs defs{
      {"Security", SecurityTuple{}, true},
      {"Price", 0.0, false, 0, 10000000, 7},
      {"ValidSeconds", 300, true, 60},
      {"MinSize", 0, false, 0, 10000000},
      {"MaxFloor", 0, false, 0, 10000000},
      {"MaxPov", 0.0, false, 0, 1, 2},
      {"Aggression", ParamDef::ValueVector{"Low", "Medium", "High", "Highest"},
       true},
      {"InternalCross", ParamDef::ValueVector{"Yes", "No"}, false},
  };
  return defs;
}

double TWAP::GetLeaves() noexcept {
  auto ratio = std::min(
      1., (GetTime() - begin_time_ + 1) / (1. * (end_time_ - begin_time_) + 1));
  auto expect = st_.qty * ratio;
  return expect - inst_->total_exposure();
}

void TWAP::Timer() {
  auto now = GetTime();
  if (now > end_time_) {
    Stop();
    return;
  }
  SetTimeout([this]() { Timer(); }, 1);
  if (!inst_->sec().IsInTradePeriod()) return;

  auto& md = this->md();
  auto bid = md.quote().bid_price;
  auto ask = md.quote().ask_price;
  // md.trade.close may be not rounded
  auto last_px = RoundPrice(md.trade.close);
  auto mid_px = 0.;
  if (ask > bid && bid > 0) mid_px = RoundPrice((ask + bid) / 2);

  Contract c;
  switch (agg_) {
    case kAggLow:
      if (IsBuy(st_.side)) {
        if (bid > 0)
          c.price = bid;
        else if (last_px > 0)
          c.price = last_px;
        else
          return;
      } else {
        if (ask > 0)
          c.price = ask;
        else if (last_px > 0)
          c.price = last_px;
        else
          return;
      }
      break;
    case kAggMedium:
      if (mid_px > 0) {
        c.price = mid_px;
        break;
      }  // else go to kAggHigh
    case kAggHigh:
      if (IsBuy(st_.side)) {
        if (ask > 0) {
          c.price = ask;
          break;
        }  // else go to kAggHighest
      } else {
        if (bid > 0) {
          c.price = bid;
          break;
        }  // else go to kAggHighest
      }
    case kAggHighest:
    default:
      c.type = kMarket;
      break;
  }
  if (c.type != kMarket && price_ > 0 &&
      ((IsBuy(st_.side) && c.price > price_) ||
       (!IsBuy(st_.side) && c.price < price_))) {
    c.price = price_;
  }
  if (not_lower_than_last_px_ && c.price < last_px) c.price = last_px;

  if (!inst_->active_orders().empty()) {
    for (auto ord : inst_->active_orders()) {
      if (c.price <= 0 || c.price == ord->price) continue;
      if (IsBuy(st_.side)) {
        if (ord->price < bid) Cancel(*ord);
      } else {
        if (ask > 0 && ord->price > ask) Cancel(*ord);
      }
    }
    return;
  }

  auto volume = md.trade.volume - initial_volume_;
  if (volume > 0 && max_pov_ > 0) {
    if (inst_->total_qty() - inst_->total_cx_qty() > max_pov_ * volume) return;
  }
  auto leaves = GetLeaves();
  if (leaves <= 0) return;
  auto total_leaves = st_.qty - inst_->total_exposure();
  auto lot_size = inst_->sec().lot_size;
  auto odd_ok = inst_->sec().exchange->odd_lot_allowed || (lot_size <= 0);
  if (lot_size <= 0) lot_size = std::max(1, min_size_);
  auto max_qty =
      odd_ok ? total_leaves : std::floor(total_leaves / lot_size) * lot_size;
  if (max_qty <= 0) return;
  auto would_qty = std::ceil(leaves / lot_size) * lot_size;
  if (would_qty < min_size_) would_qty = min_size_;
  if (max_floor_ > 0 && would_qty > max_floor_) would_qty = max_floor_;
  if (would_qty > max_qty) would_qty = max_qty;
  c.side = st_.side;
  c.qty = would_qty;
  c.sub_account = st_.acc;
  c.position_effect = st_.position_effect;
  Place(&c);
}

}  // namespace opentrade
