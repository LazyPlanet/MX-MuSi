#include <vector>
#include <algorithm>

#include <boost/asio.hpp>
#include <cpp_redis/cpp_redis>

#include "Room.h"
#include "Clan.h"
#include "Game.h"
#include "MXLog.h"
#include "CommonUtil.h"
#include "RedisManager.h"
#include "Timer.h"
#include "Activity.h"

namespace Adoter
{

extern const Asset::CommonConst* g_const;

//
//房间
//

Asset::ERROR_CODE Room::TryEnter(std::shared_ptr<Player> player)
{
	std::lock_guard<std::mutex> lock(_mutex);

	if (!player) return Asset::ERROR_INNER;

	if (player->GetLocalRoomID() == GetID()) return Asset::ERROR_SUCCESS; //重入

	auto it = std::find_if(_players.begin(), _players.end(), [player](std::shared_ptr<Player> p) {
				if (!p) return false;
				return player->GetID() == p->GetID();
			});

	if (it != _players.end()) 
	{
		return Asset::ERROR_ROOM_HAS_BEEN_IN; //已经在房间
	}
	
	if (IsFull()) 
	{
		return Asset::ERROR_ROOM_IS_FULL; //房间已满
	}
	else if (HasBeenOver()) 
	{
		return Asset::ERROR_ROOM_BEEN_OVER; //战局结束
	}
	else if (HasDisMiss()) 
	{
		return Asset::ERROR_ROOM_BEEN_DISMISS; //房间已经解散
	}

	DEBUG("玩家:{} 进入房间:{} 成功", player->GetID(), GetID());

	return Asset::ERROR_SUCCESS;
}

//
//玩家退出，位置需要为后面加入的玩家空置出来
//
//防止玩家进入和退出，位置不一致的情况
//
bool Room::IsFull() 
{ 
	if (_players.size() < (size_t)MAX_PLAYER_COUNT) return false;

	for (auto player : _players)
	{
		if (!player) return false;
	}

	return true;
} 
	
bool Room::IsEmpty()
{
	if (_players.size() == 0) return true;
	
	for (auto player : _players)
	{
		if (player) return false;
	}

	return true;
}

bool Room::Enter(std::shared_ptr<Player> player)
{
	if (!player) return false;
	
	auto enter_status = TryEnter(player);
	
	std::lock_guard<std::mutex> lock(_mutex);

	if (enter_status != Asset::ERROR_SUCCESS && enter_status != Asset::ERROR_ROOM_HAS_BEEN_IN) 
	{
		LOG(ERROR, "玩家:{} 无法进入房间:{} 原因:{}", player->GetID(), GetID(), enter_status);
		return false; //进入房间之前都需要做此检查，理论上不会出现
	}

	if (MAX_PLAYER_COUNT == _players.size())
	{
		for (size_t i = 0; i < _players.size(); ++i)
		{
			auto& player_in = _players[i];

			if (!player_in && enter_status != Asset::ERROR_ROOM_HAS_BEEN_IN) //当前玩家尚未在房间内，则选择新位置
			{
				player_in = player;
				player->SetPosition((Asset::POSITION_TYPE)(i + 1)); //设置位置
				break;
			}
			else if (player_in && player_in->GetID() == player->GetID()) //当前还在房间内
			{
				OnReEnter(player);

				return true;
			}
		}
	}
	else if (enter_status != Asset::ERROR_ROOM_HAS_BEEN_IN)
	{
		_players.push_back(player); //进入房间
		player->SetPosition((Asset::POSITION_TYPE)_players.size()); //设置位置
	}
	
	UpdateClanStatus(); //茶馆房间状态同步
	
	return true;
}
	
void Room::OnReEnter(std::shared_ptr<Player> op_player)
{
	if (!op_player) return;
	
	op_player->SetOffline(false); //回到房间内

	//
	//同步玩家信息
	//
	SyncRoom();

	if (!HasStarted() || HasBeenOver()/*单纯记录局数不能判定对局已经结束*/) return; //尚未开局或者已经对局结束

	//
	//房间内玩家数据推送
	//
	Asset::RoomAll message;
	message.set_current_rounds(_games.size());
	//message.set_current_rounds(_real_gamed_count);
	message.set_zhuang_position(Asset::POSITION_TYPE(_banker_index + 1));
	
	for (const auto& record : _history.list())
	{	
		auto hist_record = message.mutable_list()->Add();
		hist_record->CopyFrom(record);

		for (int32_t i = 0; i < hist_record->list().size(); ++i)
		{
			if (message.player_brief_list().size() < MAX_PLAYER_COUNT)
			{
				auto player_brief = message.mutable_player_brief_list()->Add();
				player_brief->set_player_id(hist_record->list(i).player_id());
				player_brief->set_nickname(hist_record->list(i).nickname());
				player_brief->set_headimgurl(hist_record->list(i).headimgurl());
			}

			hist_record->mutable_list(i)->clear_nickname();
			hist_record->mutable_list(i)->clear_headimgurl();
			hist_record->mutable_list(i)->mutable_details()->Clear();
		}
	}

	if (!_game)
	{
		op_player->SendProtocol(message);
		return;
	}

	//牌局通用信息
	message.set_curr_operator_position(Asset::POSITION_TYPE(_game->GetCurrPlayerIndex() + 1));
	message.set_remain_cards_count(_game->GetRemainCount());

	//
	//牌局相关数据推送
	//

	if (op_player->HasTingPai() && !HasAnbao()) //非暗宝
	{
		message.mutable_baopai()->CopyFrom(_game->GetBaoPai()); //宝牌
	}
	
	if (HasHuiPai()) 
	{
		message.mutable_huipai()->CopyFrom(_game->GetFanPai()); //翻牌//不是会儿牌
	}

	if (!op_player->CheckCardsInhand())
	{
		message.mutable_zhuapai()->CopyFrom(op_player->GetZhuaPai()); //上次抓牌，提示Client显示
	}

	for (auto saizi : _game->GetSaizi())
		message.mutable_saizi_random_result()->Add(saizi);

	for (auto player : _players)
	{
		if (!player) continue;

		auto player_list = message.mutable_player_list()->Add();
		player_list->set_player_id(player->GetID());
		player_list->set_position(player->GetPosition());
		player_list->set_pai_count_inhand(player->GetCardCount());

		if (player->HasTingPai()) player_list->set_tingpai(true);

		if (op_player->GetID() == player->GetID())
		{
			const auto& cards_inhand = player->GetCardsInhand();
			for (const auto& cards : cards_inhand)
			{
				if (cards.second.size() == 0) continue;

				auto pais = player_list->mutable_pai_inhand()->Add();
				pais->set_card_type((Asset::CARD_TYPE)cards.first);
				for (auto card_value : cards.second) pais->mutable_cards()->Add(card_value);
			}
		}

		auto cards_outhand = player->GetCardsOuthand();
		for (auto cards : cards_outhand)
		{
			if (cards.second.size() == 0) continue;

			for (auto card_value : cards.second) 
			{
				auto pai = player_list->mutable_pai_outhand()->Add();
				pai->set_card_type((Asset::CARD_TYPE)cards.first);
				pai->set_card_value(card_value);
			}
		}

		const auto& minggang = player->GetMingGang();
		for (auto gang : minggang)
		{
			auto pai = player_list->mutable_minggang_list()->Add();
			pai->CopyFrom(gang);
		}
		
		const auto& angang = player->GetAnGang();
		for (auto gang : angang)
		{
			auto pai = player_list->mutable_angang_list()->Add();
			pai->CopyFrom(gang);
		}
		
		const auto& zhuifeng_gang = player->GetZhuiFengGang();
		for (auto gang : zhuifeng_gang)
		{
			auto pai = player_list->mutable_zhuifeng_gang_list()->Add();
			pai->CopyFrom(gang);
		}
		
		const auto& zhuijian_gang = player->GetZhuiJianGang();
		for (auto gang : zhuijian_gang)
		{
			auto pai = player_list->mutable_zhuijian_gang_list()->Add();
			pai->CopyFrom(gang);
		}

		player_list->set_fenggang_count(player->GetFengGang());
		player_list->set_jiangang_count(player->GetJianGang());

		auto pai_pool = player->GetCardsPool();
		for (auto pai_element : pai_pool)
		{
			auto pai = player_list->mutable_pai_pool()->Add();
			pai->CopyFrom(pai_element);
		}
	}

	op_player->SendProtocol(message);

	_game->OnPlayerReEnter(op_player); //玩家操作
}

void Room::OnPlayerLeave(int64_t player_id)
{
	UpdateClanStatus(); //茶馆房间状态同步

	SyncRoom(); //同步当前房间内玩家数据
}

std::shared_ptr<Player> Room::GetHoster()
{
	return _hoster;
}

bool Room::IsHoster(int64_t player_id)
{
	if (_stuff.clan_id()) //茶馆房，老板可以作为房主逻辑
	{
		Asset::Clan clan;
		bool has_clan = ClanInstance.GetCache(_stuff.clan_id(), clan);
		if (has_clan && clan.hoster_id() == player_id) return true;
	}

	auto host = GetHoster();
	if (!host) return false;

	return host->GetID() == player_id;
}

std::shared_ptr<Player> Room::GetPlayer(int64_t player_id)
{
	for (auto player : _players)
	{
		if (player->GetID() == player_id) return player;
	}

	return nullptr;
}

void Room::OnPlayerOperate(std::shared_ptr<Player> player, pb::Message* message)
{
	//std::lock_guard<std::mutex> lock(_mutex);

	if (!player || !message) return;

	auto game_operate = dynamic_cast<Asset::GameOperation*>(message);
	if (!game_operate) return;

	BroadCast(game_operate); //广播玩家操作
	
	switch(game_operate->oper_type())
	{
		case Asset::GAME_OPER_TYPE_START: //开始游戏：其实是个准备
		{
			if (!CanStarGame()) return;

			_game = std::make_shared<Game>();

			_game->Init(shared_from_this()); //洗牌

			_game->Start(_players, _stuff.room_id(), _games.size()); //开始游戏

			if (_games.size() == 0) _real_gamed_count = 1; //真正的圈数//换庄才增加，和真正线下麻将类似

			_games.push_back(_game); //游戏缓存

			OnGameStart();
		}
		break;

		case Asset::GAME_OPER_TYPE_LEAVE: //离开游戏
		{
			if (IsFriend() && !HasDisMiss() && HasStarted() && !HasBeenOver()) return; //好友房没有开局，且没有对战完，则不允许退出
			
			if (IsMatch() && IsGaming()) return; //非好友房结束可以直接退出

			if (IsHoster(player->GetID()) && !IsClan()) //非茶馆房间
			{
				if (_games.size() == 0) //尚未开局
				{
					KickOutPlayer(); //如果房主离开房间，且此时尚未开局，则直接解散
				}
				else
				{
					Remove(player->GetID(), Asset::GAME_OPER_TYPE_LEAVE); //玩家退出房间
				}
			}
			else if (IsEmpty())
			{
				player->OnLeaveRoom(Asset::GAME_OPER_TYPE_LEAVE); //玩家退出房间
			}
			else
			{
				Remove(player->GetID(), Asset::GAME_OPER_TYPE_LEAVE); //玩家退出房间
			}
		}
		break;
		
		case Asset::GAME_OPER_TYPE_KICKOUT: //踢人
		{
			if (!IsHoster(player->GetID()))
			{
				player->AlertMessage(Asset::ERROR_ROOM_NO_PERMISSION);
				return;
			}

			Remove(game_operate->destination_player_id(), Asset::GAME_OPER_TYPE_KICKOUT); //玩家退出房间
		}
		break;

		case Asset::GAME_OPER_TYPE_DISMISS_AGREE: //解散
		{
			/*
			auto curr_time = CommonTimerInstance.GetTime();

			if (_dismiss_cooldown > 0 && curr_time <= _dismiss_cooldown) 
			{
				player->AlertMessage(Asset::ERROR_ROOM_DISMISS_COOLDOWN);
				return;
			}
			*/

			OnDisMiss(player->GetID(), game_operate);

			//
			//如果房主发起解散房间，且此时尚未开局，则直接解散
			//
			if (IsHoster(player->GetID()) && _games.size() == 0) //GMT开房没有房主
			{
				KickOutPlayer();
			}
			else if (IsGmtOpened() && (!HasStarted() || HasBeenOver()))
			{
				Remove(player->GetID(), Asset::GAME_OPER_TYPE_LEAVE); //玩家退出房间
			}
			else if (IsEmpty())
			{
				player->OnLeaveRoom(Asset::GAME_OPER_TYPE_DISMISS_AGREE); //玩家退出房间
			}
			else if (CanDisMiss()) 
			{
				DoDisMiss();
			}
		}
		break;

		case Asset::GAME_OPER_TYPE_DISMISS_DISAGREE: //不解散
		{
			OnDisMiss(player->GetID(), game_operate);

			ClearDisMiss();
		}
		break;
		
		default:
		{
		}
		break;
	}
}
	
int32_t Room::GetRemainCount() 
{ 
	//return _stuff.options().open_rands() - _games.size(); 
	return _stuff.options().open_rands() - _real_gamed_count + 1; 
}
	
bool Room::HasBeenOver() 
{ 
	if (!IsFriend()) return false;

	return !_game && GetRemainCount() <= 0; 
}

bool Room::HasLaw(Asset::ROOM_EXTEND_TYPE type)
{
	const auto& city_type = GetCity(); //房间归属城市

	if (city_type == Asset::CITY_TYPE_CHAOYANG && (type == Asset::ROOM_EXTEND_TYPE_28ZUOZHANG || type == Asset::ROOM_EXTEND_TYPE_SIGUIYI || 
			type == Asset::ROOM_EXTEND_TYPE_HUANGZHUANGHUANGGANG/*朝阳流局直接荒庄荒杠*/))
	{
		return true; //朝阳支持
	}

	if (city_type == Asset::CITY_TYPE_CHAOYANG && type == Asset::ROOM_EXTEND_TYPE_DUIBAO)
	{
		return true; //朝阳支持进宝
	}
	
	if ((city_type == Asset::CITY_TYPE_CHAOYANG || city_type == Asset::CITY_TYPE_JIANPING || city_type == Asset::CITY_TYPE_YINGKOU) && 
			type == Asset::ROOM_EXTEND_TYPE_GANGJIAFEN)
	{
		return true; //朝阳、营口、建平支持杠分
	}

	if (city_type == Asset::CITY_TYPE_JIANPING && type == Asset::ROOM_EXTEND_TYPE_JIAHU)
	{
		return true; //建平支持
	}
	
	if (city_type == Asset::CITY_TYPE_YINGKOU && type == Asset::ROOM_EXTEND_TYPE_HUANGZHUANGHUANGGANG)
	{
		return false; //营口流局,杠依然算分
	}

	if (city_type != Asset::CITY_TYPE_JIAMUSI && type == Asset::ROOM_EXTEND_TYPE_ZHUANGJIABEI)
	{
		return true; //佳木斯，庄不一定算番
	}
	
	if (city_type == Asset::CITY_TYPE_JIAMUSI && type == Asset::ROOM_EXTEND_TYPE_JIAHU)
	{
		return true; //佳木斯，带夹胡
	}

	auto it = std::find(_stuff.options().extend_type().begin(), _stuff.options().extend_type().end(), type);
	if (it == _stuff.options().extend_type().end()) return false; //常规规则检查

	return true;
}
	
bool Room::HasAnbao()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_ANBAO);
}

bool Room::HasHuiPai()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_HUIPAI);
}

bool Room::HasJueTouHui()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_JUETOUHUI);
}

bool Room::HasQiDui()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_QIDUI);
}

bool Room::HasBaopai()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_BAOPAI);
}

bool Room::HasZhang28()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_28ZUOZHANG);
}

//
//中发白其中之一只要碰就算明飘
//
//本局必须胡飘，不勾选则正常
//
bool Room::HasMingPiao()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_MINGPIAO);
}

//
//流局杠分依然算
//
bool Room::HasHuangZhuang()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_HUANGZHUANGHUANGGANG);
}

//
//手里有4张一样的不可上听胡牌，杠除外
//
bool Room::HasSiGuiYi()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_SIGUIYI);
}

bool Room::HasYiBianGao()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_YIBIANGAO);
}

bool Room::HasZhanLi()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_ZHANLIHU);
}

bool Room::HasJiaHu()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_JIAHU);
}

bool Room::HasXuanFengGang()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_XUANFENGGANG);
}

bool Room::HasZhuiFengGang()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_ZHUIFENGGANGA);
}

bool Room::HasDuanMen()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_DUANMEN);
}

bool Room::HasQingYiSe()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_QIYISE);
}

//
//谁点谁给自己的点炮钱（不用帮其他两家给）
//
//其他两家不掏钱（别人杠要帮付，点杠不帮付）
//
bool Room::HasYiJiaFu()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_YIJIAFU);
}

bool Room::HasBaoSanJia()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_BAOSANJIA);
}

bool Room::HasZhuangDouble()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_ZHUANGJIABEI);
}

bool Room::HasGuaDaFeng()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_GUADAFENG);
}

bool Room::HasJiaBao()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_JIABAO);
}

bool Room::HasBuJiaBuHu()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_BUJIABUHU);
}

bool Room::HasManTianFei()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_MANTIANFEI);
}

bool Room::HasGangJiaFen()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_GANGJIAFEN);
}

bool Room::HasKaiPaiZha()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_KAIPAIZHA);
}

bool Room::HasDuiBao()
{
	return HasLaw(Asset::ROOM_EXTEND_TYPE_DUIBAO);
}
	
//
//获取番数
//
int32_t Room::GetMultiple(int32_t fan_type)
{
	const auto fan_asset = GetFan();
	if (!fan_asset) return 0;

	auto it = std::find_if(fan_asset->fans().begin(), fan_asset->fans().end(), [fan_type](const Asset::RoomFan_FanElement& element){
			return fan_type == element.fan_type();
	});
	if (it == fan_asset->fans().end()) return 0;

	return pow(2, it->multiple());
}
	
const Asset::RoomFan* Room::GetFan()
{
	auto city_type = _stuff.options().city_type();

	const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ROOM_FAN);
	auto it = std::find_if(messages.begin(), messages.end(), [city_type](pb::Message* message){
				auto room_fan = dynamic_cast<Asset::RoomFan*>(message);
				if (!room_fan) return false;

				return room_fan->city_type() == city_type;
			});
	if (it == messages.end()) return nullptr;

	const auto room_fan = dynamic_cast<const Asset::RoomFan*>(*it);
	if (!room_fan) return nullptr;

	return room_fan;
}

bool Room::Remove(int64_t player_id, Asset::GAME_OPER_TYPE reason)
{
	std::lock_guard<std::mutex> lock(_mutex);

	for (size_t i = 0; i < _players.size(); ++i)
	{
		auto& player = _players[i];
		if (!player) continue;

		if (player->GetID() != player_id) continue;
			
		player->OnLeaveRoom(reason); //玩家退出房间

		//player = nullptr; 
		
		player.reset();

		//_players.erase(it); //删除玩家

		OnPlayerLeave(player_id); //玩家离开房间
		
		//DEBUG("玩家:{}离开房间:{}", player_id, _stuff.room_id());

		return true;
	}

	return false;
}
	
void Room::OnPlayerStateChanged()
{
	//std::lock_guard<std::mutex> lock(_mutex);

	Asset::RoomInformation message;
	message.set_sync_type(Asset::ROOM_SYNC_TYPE_STATE_CHANGED);
			
	for (auto player : _players)
	{
		if (!player) continue;

		auto p = message.mutable_player_list()->Add();
		p->set_player_id(player->GetID());
		p->set_position(player->GetPosition());
		p->set_oper_type(player->GetOperState());
	}

	BroadCast(message);
}

void Room::OnGameStart()
{
	std::lock_guard<std::mutex> lock(_mutex);

	Asset::GameStart game_start;
	game_start.set_total_rounds(_stuff.options().open_rands());
	game_start.set_current_rounds(_games.size());
	//game_start.set_current_rounds(_real_gamed_count);

	BroadCast(game_start);

	for (auto player : _players)
	{
		if (!player) continue;

		player->SetOperState(Asset::GAME_OPER_TYPE_ONLINE);
	}
	
	UpdateClanStatus(); //茶馆房间状态同步
}
	
void Room::AddHupai(int64_t player_id) 
{ 
	//std::lock_guard<std::mutex> lock(_mutex);

	if (player_id <= 0) return;

	++_hupai_players[player_id]; 

	auto player = GetPlayer(player_id);
	if (!player) return;

	player->AddTotalWinRounds();
	player->SetStreakWins(1);
}

void Room::OnClanOver()
{
	if (!IsClan()) return;
	
	//RedisInstance.SaveRoomHistory(_stuff.room_id(), _history); //茶馆房间，存储战绩信息
	//if (_history.list().size() == 0) return;
	
	Asset::ClanRoomStatusChanged proto;
	proto.set_created_time(_created_time);
	proto.mutable_room()->CopyFrom(_stuff);
	proto.set_status(Asset::CLAN_ROOM_STATUS_TYPE_OVER);
	proto.mutable_player_list()->CopyFrom(_history.player_brief_list());
	proto.set_games_count(GetGamesCount()); //开局数量

	WorldInstance.BroadCast2CenterServer(proto); //通知茶馆房间结束
}

void Room::OnGameOver(int64_t player_id)
{
	if (_game) _game.reset();

	if (!IsFriend()) return; //非好友房没有总结算
	
	UpdateClanStatus(); //茶馆房间状态同步
	
	AddHupai(player_id); //记录

	if (player_id != 0 && _banker != player_id) 
	{
		++_real_gamed_count; //换庄
	
		auto city_type = GetCity();

		_banker_index = (_banker_index + 1) % MAX_PLAYER_COUNT; //下庄

		if (city_type == Asset::CITY_TYPE_JIANPING) //建平
		{
			_banker_index = GetPlayerOrder(player_id); //谁胡下一局谁坐庄
		}
		
		auto player = GetPlayer(player_id);
		if (player) player->SetStreakWins(_streak_wins[player_id]); //最高连胜

		_streak_wins[player_id] = 0;
	}
	else
	{
		++_streak_wins[player_id]; //连庄
	}
		
	DEBUG("当前胡牌玩家:{}, 当前庄家:{} 当前实际局数:{} 即将开始局数:{}", player_id, _banker, _games.size(), _real_gamed_count);

	if (!HasBeenOver() && !HasDisMiss()) return; //没有对局结束，且没有解散房间

	if (_games.size() == 0) return; //没有对局
	
	for (auto player : _players)
	{
		if (!player) continue;
		if (_history.list().size()) player->AddRoomRecord(GetID());
	}

	//
	//总结算界面弹出
	//
	Asset::RoomCalculate message;
	message.set_calculte_type(Asset::CALCULATE_TYPE_FULL);
	if (HasDisMiss()) message.set_calculte_type(Asset::CALCULATE_TYPE_DISMISS);

	for (auto player : _players)
	{
		if (!player) continue;

		auto player_id = player->GetID();

		auto record = message.mutable_record()->Add();
		record->set_player_id(player_id);
		record->set_nickname(player->GetNickName());
		record->set_headimgurl(player->GetHeadImag());

		record->set_pk_count(_games.size());
		record->set_banker_count(_bankers[player_id]);
		record->set_win_count(_hupai_players[player_id]);
		record->set_dianpao_count(_dianpao_players[player_id]);
		record->set_loubao_count(_loubao_players[player_id]);
		record->set_jinbao_count(_jinbao_players[player_id]);

		for(int i = 0; i < _history.list().size(); ++i)
			for (int j = 0; j < _history.list(i).list().size(); ++j)
				if (player_id == _history.list(i).list(j).player_id())
					record->set_score(record->score() + _history.list(i).list(j).score());

		auto player_brief = _history.mutable_player_brief_list()->Add();
		player_brief->set_player_id(player_id);
		player_brief->set_nickname(player->GetNickName());
		player_brief->set_headimgurl(player->GetHeadImag());
		player_brief->set_score(record->score());
		player_brief->set_hupai_count(record->win_count());
		player_brief->set_dianpao_count(record->dianpao_count());

		player->AddRoomScore(record->score()); //总积分
	}

	LOG(INFO, "房间:{} 整局结算，房间局数:{} 实际局数:{} 连庄局数:{} 结算数据:{}", 
			_stuff.room_id(), _stuff.options().open_rands(), _games.size(), _real_gamed_count, message.ShortDebugString());

	for (auto player : _players)
	{
		if (!player/* || player->IsOffline()*/) continue;

		player->SendProtocol(message);
	}
	
	OnClanOver(); //茶馆房间数据同步
	
	_history.Clear();
	_bankers.clear();
	_hupai_players.clear();
	_dianpao_players.clear();
	_streak_wins.clear();
	_loubao_players.clear();
	_jinbao_players.clear();
	
	/*
	Asset::ClanRoomStatusChanged proto;
	proto.set_created_time(_created_time);
	proto.mutable_room()->CopyFrom(_stuff);
	proto.set_status(Asset::CLAN_ROOM_STATUS_TYPE_OVER);
	WorldInstance.BroadCast2CenterServer(proto); //通知茶馆房间结束
	*/
}

void Room::AddGameRecord(const Asset::GameRecord& record)
{
	_history.mutable_list()->Add()->CopyFrom(record);
	RedisInstance.SaveRoomHistory(_stuff.room_id(), _history);

	LOG(INFO, "存储房间:{} 历史战绩:{}", _stuff.room_id(), _history.ShortDebugString());
}

void Room::BroadCast(pb::Message* message, int64_t exclude_player_id)
{
	if (!message) return;
			
	for (auto player : _players)
	{
		if (!player) continue; //可能已经释放//或者退出房间

		if (exclude_player_id == player->GetID()) continue;

		player->SendProtocol(message);
	}
}
	
void Room::BroadCast(pb::Message& message, int64_t exclude_player_id)
{
	BroadCast(&message, exclude_player_id);
}
	
void Room::OnRemove()
{
	std::lock_guard<std::mutex> lock(_mutex);

	WARN("房间:{} 删除成功", _stuff.room_id());

	for (auto& player : _players)
	{
		if (!player) continue;

		player->OnRoomRemoved();
		player.reset();
	}
	
	/*
	Asset::ClanRoomStatusChanged proto;
	proto.mutable_room()->CopyFrom(_stuff);
	proto.set_created_time(_created_time);
	proto.set_status(Asset::CLAN_ROOM_STATUS_TYPE_OVER);
	WorldInstance.BroadCast2CenterServer(proto); //通知茶馆房间结束
	*/

	OnClanOver();
}

void Room::OnDisMiss(int64_t player_id, pb::Message* message)
{
	if (!message) return;

	if ((IsGmtOpened() || IsClan()) && (!HasStarted() || HasBeenOver())) return; //代开房//茶馆房没开局不允许解散

	if (_dismiss_time == 0) 
	{
		_dismiss_time = CommonTimerInstance.GetTime() + g_const->room_dismiss_timeout();
		_dismiss_cooldown = CommonTimerInstance.GetTime() + g_const->room_dismiss_cooldown();
	}

	Asset::RoomDisMiss proto;

	for (auto player : _players)
	{
		if (!player) continue;

		if (Asset::GAME_OPER_TYPE_DISMISS_AGREE != player->GetOperState() && 
				Asset::GAME_OPER_TYPE_DISMISS_DISAGREE != player->GetOperState()) continue;

		auto list = proto.mutable_list()->Add();
		list->set_player_id(player->GetID());
		list->set_position(player->GetPosition());
		list->set_oper_type(player->GetOperState());
	}

	DEBUG("玩家:{} 解散房间:{} 操作:{}", player_id, _stuff.room_id(), message->ShortDebugString());

	BroadCast(proto); //投票状态
}

void Room::DoDisMiss()
{
	DEBUG("房间:{} 解散成功", _stuff.room_id());

	_is_dismiss = true;
					
	if (_game) { _game->OnGameOver(0); } //当前局数结束
	else { OnGameOver(); }
}
	
void Room::KickOutPlayer(int64_t player_id)
{
	for (auto player : _players)
	{
		if (!player) continue;

		if (player_id != 0 && player->GetID() != player_id) continue;

		Remove(player->GetID(), Asset::GAME_OPER_TYPE_HOSTER_DISMISS); //踢人
	}

	//RoomInstance.Remove(GetID());
	_is_dismiss = true;
}
	
void Room::SyncRoom()
{
	//std::lock_guard<std::mutex> lock(_mutex); //防止死锁
	
	Asset::RoomInformation message;
	message.set_sync_type(Asset::ROOM_SYNC_TYPE_NORMAL);
			
	for (auto player : _players)
	{
		if (!player) continue;

		auto p = message.mutable_player_list()->Add();
		p->set_position(player->GetPosition());
		p->set_player_id(player->GetID());
		p->set_oper_type(player->GetOperState());
		p->mutable_common_prop()->CopyFrom(player->CommonProp());
		p->mutable_wechat()->CopyFrom(player->GetWechat());
		p->set_ip_address(player->GetIpAddress());
		p->set_voice_member_id(player->GetVoiceMemberID());
	
		for (auto dis_player : _players)
		{
			if (!dis_player || dis_player->GetID() == player->GetID()) continue;

			auto dis_element = p->mutable_dis_list()->Add();
			dis_element->set_position(dis_player->GetPosition());

			auto distance = RedisInstance.GetDistance(dis_player->GetID(), player->GetID());
			dis_element->set_distance(distance);

			//DEBUG("获取玩家{}和玩家{}之间的距离:{}", dis_player->GetID(), player->GetID(), distance);
		}
	}

	DEBUG("同步房间数据:{}", message.ShortDebugString());

	BroadCast(message);
}

void Room::OnCreated(std::shared_ptr<Player> hoster) 
{ 
	_hoster = hoster;
	if (hoster) _hoster_id = hoster->GetID();

	_created_time = CommonTimerInstance.GetTime(); //创建时间
	SetExpiredTime(_created_time + g_const->room_last_time());
	
	_history.set_room_id(GetID());
	_history.set_create_time(CommonTimerInstance.GetTime()); //创建时间
	_history.mutable_options()->CopyFrom(GetOptions());
	
	UpdateClanStatus(); //茶馆房间状态同步

	LOG(INFO, "玩家:{} 创建房间:{} 玩法:{}成功", _hoster_id, _stuff.room_id(), _stuff.ShortDebugString());
}
	
bool Room::CanStarGame()
{
	if (IsFriend() && !_hoster && !_gmt_opened) return false;

	if (_players.size() != MAX_PLAYER_COUNT) return false;

	for (auto player : _players)
	{
		if (!player) return false;

		if (!player->IsReady()) return false; //玩家都是准备状态
	}
	
	auto room_type = _stuff.room_type();
	
	//
	//开始游戏，消耗房卡//欢乐豆
	//
	if (Asset::ROOM_TYPE_FRIEND == room_type)
	{
		if (GetRemainCount() <= 0) 
		{
			LOG(ERROR, "房间:{}牌局结束，不能继续进行游戏，总局数:{} 当前局数:{}", _stuff.room_id(), _stuff.options().open_rands(), _games.size());
			return false;
		}

		auto activity_id = g_const->room_card_limit_free_activity_id();
		if (ActivityInstance.IsOpen(activity_id)) return true;

		if (IsGmtOpened()) 
		{
			LOG(INFO, "GMT开房，不消耗房卡数据:{}", _stuff.ShortDebugString());
			return true;
		}
			
		const auto room_card = dynamic_cast<const Asset::Item_RoomCard*>(AssetInstance.Get(g_const->room_card_id()));
		if (!room_card || room_card->rounds() <= 0) return false;

		auto consume_count = GetOptions().open_rands() / room_card->rounds(); //待消耗房卡数

		if (IsClan()) //茶馆：开局消耗，到中心服务器消耗
		{
			if (_games.size()) return true; //已经开局，不再进行房卡检查

			Asset::Clan clan;
			auto has_record = ClanInstance.GetClan(_stuff.clan_id(), clan);
			if (!has_record || clan.room_card_count() < consume_count) return false;

			if (_games.size() == 0) OnClanCreated(); //茶馆房
			return true;
		}

		if (!_hoster) return false; //没有房主，正常消耗

		if (_hoster && _games.size() == 0) //开局消耗
		{
			auto pay_type = GetOptions().pay_type(); //付费方式
		
			switch (pay_type)
			{
				case Asset::ROOM_PAY_TYPE_HOSTER: //房主付卡
				{
					if (!_hoster->CheckRoomCard(consume_count)) 
					{
						_hoster->AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //房卡不足
						return false;
					}
					_hoster->ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_OPEN_ROOM, consume_count); //消耗
				}
				break;
				
				case Asset::ROOM_PAY_TYPE_AA: //AA付卡
				{
					consume_count = consume_count / MAX_PLAYER_COUNT; //单人付卡数量

					for (auto player : _players) //房卡检查
					{
						if (!player) return false;

						if (!player->CheckRoomCard(consume_count)) 
						{
							player->AlertMessage(Asset::ERROR_ROOM_CARD_NOT_ENOUGH); //理论上一定会过，玩家进入AA付卡已经前置检查
							return false;
						}
					}
					
					for (auto player : _players) //房卡消耗
					{
						if (!player) return false;
						player->ConsumeRoomCard(Asset::ROOM_CARD_CHANGED_TYPE_OPEN_ROOM, consume_count); 
					}
				}
				break;

				default:
				{
					return false;
				}
				break;
			}
		}
		else if (_games.size() == 0)
		{
			LOG(ERROR, "房间:{}尚未消耗房卡进行开房, 房主:{}", _stuff.room_id(), _hoster->GetID()); //记录
			return false;
		}
	}
	else
	{
		const auto& messages = AssetInstance.GetMessagesByType(Asset::ASSET_TYPE_ROOM);

		auto it = std::find_if(messages.begin(), messages.end(), [room_type](pb::Message* message){
			auto room_limit = dynamic_cast<Asset::RoomLimit*>(message);
			if (!room_limit) return false;

			return room_type == room_limit->room_type();
		});

		if (it == messages.end()) return false;
		
		auto room_limit = dynamic_cast<Asset::RoomLimit*>(*it);
		if (!room_limit) return false;

		//
		//欢乐豆检查
		//
		for (auto player : _players)
		{
			if (!player) 
			{
				DEBUG_ASSERT(false && "开局失败，未能找到玩家");
				return false;
			}
		
			auto beans_count = player->GetHuanledou();
			if (beans_count < room_limit->cost_count()) return false;
		}
		
		//
		//欢乐豆消耗
		//
		for (auto player : _players)
		{
			if (!player) 
			{
				DEBUG_ASSERT(false && "开局失败，未能找到玩家");
				return false;
			}
			
			auto real_cost = player->ConsumeHuanledou(Asset::HUANLEDOU_CHANGED_TYPE_ROOM_TICKET, room_limit->cost_count());
			if (room_limit->cost_count() != real_cost) return false;
		}
	}
	
	return true;
}

void Room::OnClanCreated()
{
	if (!IsClan()) return;

	Asset::ClanRoomStatusChanged message;
	message.set_status(Asset::CLAN_ROOM_STATUS_TYPE_START);
	message.mutable_room()->CopyFrom(_stuff);

	WorldInstance.BroadCast2CenterServer(message);
}

bool Room::CanDisMiss()
{
	for (auto player : _players)
	{
		if (!player) return false;

		if (!player->AgreeDisMiss()) return false; 
	}

	return true;
}

void Room::ClearDisMiss()
{
	_dismiss_time = 0;
	_dismiss_cooldown = 0;
	
	for (auto player : _players)
	{
		if (!player) continue;

		if (player->AgreeDisMiss() || player->DisAgreeDisMiss()) player->ClearDisMiss(); //必须重新投票
	}

	if (!_game) SyncRoom();
}
	
bool Room::IsExpired()
{
	if (_gmt_opened) return false; //代开房不解散

	auto curr_time = CommonTimerInstance.GetTime();
	return _expired_time < curr_time;
}

void Room::Update()
{
	auto curr_time = CommonTimerInstance.GetTime();

	if (!_is_dismiss && _dismiss_time > 0 && _dismiss_time <= curr_time) //非已确认解散，系统才自动解散
	{
		DoDisMiss(); //解散
	}
}

void Room::UpdateClanStatus()
{
	if (!IsFriend()) return; //非好友房没有总结算

	if (!IsClan()) return; //非茶馆房间不同步
	
	Asset::ClanRoomSync message;

	Asset::RoomInformation room_information;
	room_information.set_sync_type(Asset::ROOM_SYNC_TYPE_QUERY); //外服查询房间信息
			
	for (const auto player : _players)
	{
		if (!player) continue;

		auto p = room_information.mutable_player_list()->Add();
		p->set_position(player->GetPosition());
		p->set_player_id(player->GetID());
		p->set_oper_type(player->GetOperState());
		p->mutable_common_prop()->CopyFrom(player->CommonProp());
		p->mutable_wechat()->CopyFrom(player->GetWechat());
		p->set_ip_address(player->GetIpAddress());
		p->set_voice_member_id(player->GetVoiceMemberID());
	}

	Asset::RoomQueryResult room_info;
	room_info.set_room_id(GetID());
	room_info.set_clan_id(GetClan());
	room_info.set_create_time(GetCreateTime());
	room_info.mutable_options()->CopyFrom(GetOptions());
	room_info.mutable_information()->CopyFrom(room_information);
	room_info.set_curr_count(GetGamesCount());

	message.set_room_status(room_info.SerializeAsString());

	DEBUG("逻辑服务器:{} 向中心服务器广播茶馆房间信息:{}", g_server_id, room_info.ShortDebugString());

	WorldInstance.BroadCast2CenterServer(message);
}
	
/////////////////////////////////////////////////////
//房间通用管理类
/////////////////////////////////////////////////////
RoomManager::RoomManager()
{
	_server_id = ConfigInstance.GetInt("ServerID", 1); 
}

std::shared_ptr<Room> RoomManager::Get(int64_t room_id)
{
	std::lock_guard<std::mutex> lock(_room_lock);

	auto it = _rooms.find(room_id);
	if (it == _rooms.end()) 
	{
		ERROR("获取房间:{} 数据失败：未找到房间", room_id);
		return nullptr;
	}
	
	return it->second;
}
	
bool RoomManager::CheckPassword(int64_t room_id, std::string password)
{
	auto room = Get(room_id);
	if (!room) return false;

	return true;
}

int64_t RoomManager::AllocRoom()
{
	return RedisInstance.CreateRoom();
}
	
std::shared_ptr<Room> RoomManager::CreateRoom(const Asset::Room& room)
{
	auto room_id = room.room_id();

	if (room_id <= 0) room_id = AllocRoom(); //如果没有房间号，则创建
	if (room_id <= 0) return nullptr;

	auto locate_room = std::make_shared<Room>(room);
	locate_room->SetID(room_id);
	locate_room->OnCreated();

	auto success = OnCreateRoom(locate_room); //成功创建房间
	if (!success) return nullptr; 

	return locate_room;
}

bool RoomManager::OnCreateRoom(std::shared_ptr<Room> room)
{
	if (!room) return false;

	auto room_id = room->GetID();

	std::lock_guard<std::mutex> lock(_room_lock);

	if (_rooms.find(room_id) != _rooms.end()) return false;
	_rooms.emplace(room_id, room);

	return true;
}

std::shared_ptr<Room> RoomManager::GetMatchingRoom(Asset::ROOM_TYPE room_type)
{
	std::lock_guard<std::mutex> lock(_match_mutex);

	auto& rooms = _matching_rooms[room_type];

	for (auto it = rooms.begin(); it != rooms.end(); ++it)
	{
		if (it->second->IsFull()) continue;
	
		return it->second; //尚未满房间
	}
			
	auto room_id = RoomInstance.AllocRoom();
	if (room_id <= 0) return nullptr; //创建

	Asset::Room room;
	room.set_room_id(room_id);
	room.set_room_type(room_type);

	auto room_ptr = std::make_shared<Room>(room);
	room_ptr->SetID(room_id);
	room_ptr->OnCreated();

	OnCreateRoom(room_ptr);
	rooms.emplace(room_id, room_ptr);

	return room_ptr;
}
	
void RoomManager::Update(int32_t diff)
{
	++_heart_count;
	
	std::lock_guard<std::mutex> lock(_room_lock);

	if (_heart_count % 1200 == 0) { DEBUG("服务器:{} 进行房间数量:{}", _server_id, _rooms.size()); } //1分钟
	if (_heart_count % 12000 == 0) UpdateMatching(); //10分钟

	if (_heart_count % 100 == 0) //5秒
	{
		for (auto it = _rooms.begin(); it != _rooms.end(); )
		{
			it->second->Update();

			if ((it->second->IsFriend() && it->second->IsExpired() && it->second->IsEmpty()) || it->second->HasDisMiss() || it->second->HasBeenOver())
			{
				it->second->OnRemove();
				it = _rooms.erase(it); //删除房间
			}
			else
			{
				++it;
			}
		}
	}
}

void RoomManager::UpdateMatching()
{
	std::lock_guard<std::mutex> lock(_match_mutex);

	Asset::MatchStatistics stats;
	RedisInstance.GetMatching(stats);

	Asset::MatchStatistics_MatchingRoom* match_stats = nullptr;

	if (stats.server_list().size() == 0)
	{
		match_stats = stats.mutable_server_list()->Add();
		match_stats->set_server_id(_server_id);
	}
	else
	{
		for (int32_t i = 0; i < stats.server_list().size(); ++i)
		{
			if (_server_id == stats.server_list(i).server_id())	
			{
				match_stats = stats.mutable_server_list(i);
				break;
			}
		}
	}

	if (!match_stats) return;

	match_stats->mutable_room_list()->Clear();

	for (auto room : _matching_rooms)
	{
		auto room_matching = match_stats->mutable_room_list()->Add();
		room_matching->set_room_type((Asset::ROOM_TYPE)room.first);
		room_matching->set_player_count(room.second.size());
	}

	RedisInstance.SaveMatching(stats); //存盘
}
	
void RoomManager::Remove(int64_t room_id)
{
	std::lock_guard<std::mutex> lock(_room_lock);

	auto it = _rooms.find(room_id);
	if (it == _rooms.end()) return;

	it->second->OnRemove();
	_rooms.erase(it);
}

int32_t Room::GetPlayerOrder(int32_t player_id)
{
	for (int i = 0; i < MAX_PLAYER_COUNT; ++i)
	{
		auto player = _players[i];
		if (!player) continue;

		if (player->GetID() == player_id) return i; //序号
	}

	return -1;
}

}
