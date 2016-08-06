#include "hivexx.h"
#include <hivex.h>
#include <algorithm>
#include <boost/format.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string.hpp>
#include <nst-log/Log.h>

using namespace std;
using namespace hivexx;
using namespace neosmart;

template<typename T>
cunique<T> make_cunique(T *t)
{
	static_assert(!is_void<T>::value, "unique_ptr of void is not valid!");
	return cunique<T>(std::forward<T*>(t), [] (T *t2) { if (t2 != nullptr) free(t2); });
}

Hive::HiveWrapper::HiveWrapper(hive_h *hive)
{
	_hive = hive;
}

Hive::HiveWrapper::~HiveWrapper()
{
	if (_hive != nullptr)
	{
		hivex_close(_hive);
	}
}

//Hive::Hive(const std::string &path)
//	: Key()
//{
	//This constructor has been deprecated
	//It is very important to handle the return errors of Load()

	//Load(path);
//}

bool Hive::Load(const std::string &path)
{
	logger.SetLogLevel(Debug);
	_path = path;
	_cachedName = string(path.c_str() + path.find_last_of("/\\") + 1);

	//Test if it physically exists first
	if (FILE *file = fopen(_path.c_str(), "r"))
	{
		fclose(file);
		_hive = hivex_open(path.c_str(), HIVEX_OPEN_WRITE | HIVEX_OPEN_VERBOSE);
	}

	_hiveWrapper = make_shared<HiveWrapper>(_hive);
	if (_hive != nullptr)
	{
		_node = hivex_root(_hive);
		return _node != 0;
	}

	logger.Error("Could not open %s", path.c_str());
	return false;
}

Hive::~Hive()
{
}

bool Hive::Save()
{
	if (hivex_commit(_hive, NULL, 0) == 0)
	{
		logger.Debug("Hive %s saved", Name().c_str());
		return true;
	}

	logger.Error("Could not save %s!", Name().c_str());
	return false;
}

Key::Key(hive_h *hive, hive_node_h node, const std::string &name)
{
	_hive = hive;
	_node = node;
	_cachedName = name;
}

const std::string &Hive::Path() const
{
	return _path;
}

const std::string &Key::Name() const
{
	return _cachedName;
}

bool Key::Exists() const
{
	//ScopeLog x(__FUNCTION__);
	return _node != 0 && _hive != nullptr;
}

template bool Key::ChangeIfNotEqualTo(const std::string &name, int&&, bool orNull);
template bool Key::ChangeIfNotEqualTo(const std::string &name, std::string&&, bool orNull);

template <typename T>
bool Key::ChangeIfNotEqualTo(const string &name, T &&compare, bool orNull)
{
	auto logMessage = str(boost::format("ChangeIfNotEqual: %1%\\%2% to %3%") % _cachedName % name.c_str() % compare);
	logger.Debug(logMessage.c_str());

	if (!Exists())
	{
		logger.Warn("ChangeIfNotEqualTo: parent key %s not found", _cachedName.c_str());
		return false;
	}

	T oldValue;
	bool found = GetValue(name, oldValue);
	if (orNull || found)
	{
		return (found && (oldValue == compare)) || SetValue(name, compare);
	}

	logger.Warn("ChangeIfNotEqualTo: value %s\\%s not found", _cachedName.c_str(), name.c_str());
	return false;
}

Key Key::GetSubkey(std::string path, bool create)
{
	if (!Exists())
	{
		logger.Warn("GetSubkey: parent key %s not found", _cachedName.c_str());
		return Key(nullptr, 0, path); //set _cachedName so we can use it in logging later
	}

	logger.Debug("GetSubkey: %s\\%s", _cachedName.c_str(), path.c_str());

	hive_node_h node = _node;
	bool done = false;
	char *stub;

	for (char *letter = stub = const_cast<char*>(path.data()); !done && node && *stub; ++letter)
	{
		if ((*letter == '\\') || (done = *letter == '\0'))
		{
			*letter = '\0';
			hive_node_h parentKey = node;
			node = hivex_node_get_child(_hive, node, stub);
			if (node == 0)
			{
				if (create)
				{
					logger.Debug("CreateSubkey: %s", stub);
					node = hivex_node_add_child(_hive, parentKey, stub);
				}
				else
				{
					logger.Info("Key not found: %s\\%s", _cachedName.c_str(), path.c_str());
					return Key(nullptr, 0, path); //set _cachedName so we can use it in logging later
				}
			}
			stub = letter + 1;
		}
	}

	return Key(_hive, node, path);
}

Key Key::CreateSubkey(const std::string &path)
{
	return GetSubkey(path, true);
}

//Returns true even if the value never existed. False only on failure to delete.
bool Key::DeleteValue(const std::string &name)
{
	if (!Exists())
	{
		logger.Warn("DeleteValue: parent key %s not found", _cachedName.c_str());
		return true; //highly-controversial, but pragmatically correct
	}

	//libhivex is non-destructive and append-only by nature
	//as such, it does not allow removal of a key, only changing its value
	//by pointing it a new value appended to the registry hive
	//We work around this by exporting the key contents, removing the entire key,
	//then re-importing the conutents minus the value we wish to remove
	auto values = GetValues();

	//We can use remove_if, but for efficiency's sake we should stop after one match
	for (auto value = values.cbegin(); value != values.cend(); ++value)
	{
		if (boost::iequals(name, value->Name.get()))
		{
			values.erase(value);
			if (SetValues(values))
			{
				logger.Debug("DeleteValue: %s\\%s deleted", _cachedName.c_str(), name.c_str());
				return true;
			}
			return false;
		}
	}

	//Value not found, but that's ok
	logger.Debug("DeleteValue: %s\\%s didn't exist", _cachedName.c_str(), name.c_str());
	return true;
}

bool Key::DeleteSubkey(const std::string &path)
{
	if (!Exists())
	{
		logger.Warn("DeleteSubkey: parent key %s not found", _cachedName.c_str());
		return true; //highly-controversial, but pragmatically correct
	}

	auto node = GetSubkey(path, false);
	if (!node.Exists())
	{
		logger.Debug("DeleteSubkey: %s\\%s didn't exist", _cachedName.c_str(), path.c_str());
		return true;
	}

	if (hivex_node_delete_child(_hive, node._node) == 0)
	{
		logger.Debug("DeleteSubkey: %s\\%s", _cachedName.c_str(), path.c_str());
		return true;
	}

	logger.Error("DeleteSubkey: %s\\%s failed", _cachedName.c_str(), path.c_str());
	return false;
}

std::vector<Key> Key::GetSubkeys()
{
	logger.Debug("GetSubkeys: %s", _cachedName.c_str());

	std::vector<Key> children;

	if (!Exists())
	{
		logger.Warn("GetSubkeys: parent key %s not found", _cachedName.c_str());
		return children;
	}

	for (auto node = hivex_node_children(_hive, _node); node && *node; ++node)
	{
		auto temp = make_cunique(hivex_node_name(_hive, *node));
		Key newKey(_hive, *node, str(boost::format("%s\\%s") % _cachedName % temp.get()));

		children.push_back(std::move(newKey));
	}

	return children;
}

bool Key::SetValue(const std::string &name, int32_t value)
{
	if (!Exists())
	{
		logger.Warn("SetValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	hive_set_value newValue = { const_cast<char *>(name.data()), hive_t_REG_DWORD, sizeof(int), (char *) &value };

	if (hivex_node_set_value(_hive, _node, &newValue, 0) == 0)
	{
		logger.Debug("SetValue %s\\%s: %i", _cachedName.c_str(), name.c_str(), value);
		return true;
	}

	logger.Error("SetValue %s\\%s: %i failed", _cachedName.c_str(), name.c_str(), value);
	return false;
}

#include <boost/locale.hpp>
bool Key::SetValue(const std::string &name, std::string value)
{
	if (!Exists())
	{
		logger.Warn("SetValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	auto wideValue = boost::locale::conv::utf_to_utf<char16_t>(value);
	hive_set_value newValue = { const_cast<char *>(name.data()), hive_t_REG_SZ, (wideValue.size() + 1) * sizeof(char16_t), const_cast<char *>(reinterpret_cast<const char *>(wideValue.data())) };

	if (hivex_node_set_value(_hive, _node, &newValue, 0) == 0)
	{
		logger.Debug("SetValue %s\\%s: %s", _cachedName.c_str(), name.c_str(), value.c_str());
		return true;
	}

	logger.Error("SetValue %s\\%s: %s failed", _cachedName.c_str(), name.c_str(), value.c_str());
	return false;
}

bool Key::SetValue(const std::string &name, std::vector<std::string> values)
{
	if (!Exists())
	{
		logger.Warn("SetValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	std::vector<char16_t> buffer;
	for (const auto &value : values)
	{
		auto wideValue = boost::locale::conv::utf_to_utf<char16_t>(value);
		buffer.insert(buffer.end(), wideValue.begin(), wideValue.end());
		buffer.push_back(u'\0');
	}
	buffer.push_back(u'\0');
	hive_set_value newValue = { const_cast<char *>(name.data()), hive_t_REG_MULTI_SZ, buffer.size() * sizeof(char16_t), const_cast<char *>(reinterpret_cast<const char *>(buffer.data())) };

	bool result = (hivex_node_set_value(_hive, _node, &newValue, 0) == 0);
	uint32_t count = 0;
	for (const auto &value : values)
	{
		logger.Debug("SetValue %s\\%s[%u]: %s%s", _cachedName.c_str(), name.c_str(), count++, value.c_str(), result ? "" : "failed");
	}
	if (count == 0)
	{
		logger.Debug("SetValue %s\\%s: <empty>", _cachedName.c_str(), name.c_str());
	}
	return result;
}

bool Key::HasValue(const string &name) const
{
	if (!Exists())
	{
		logger.Warn("HasValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	if (hivex_node_get_value(_hive, _node, name.c_str()) == 0)
	{
		logger.Debug("HasValue: %s\\%s found", _cachedName.c_str(), name.c_str());
		return true;
	}

	logger.Error("HasValue: %s\\%s not found", _cachedName.c_str(), name.c_str());
	return false;
}

bool Key::GetValue(const std::string &name, int32_t &result)
{
	if (!Exists())
	{
		logger.Warn("GetValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	hive_type type;
	size_t size = 0;
	auto value = hivex_node_get_value(_hive, _node, name.c_str());
	if (value != 0 && hivex_value_type(_hive, value, &type, &size) == 0 && type == hive_t_REG_DWORD)
	{
		result = hivex_value_dword(_hive, value);
		logger.Debug("GetValue %s\\%s: %d", _cachedName.c_str(), name.c_str(), result);
		return true;
	}
	
	logger.Error("GetValue: value %s\\%s not found", _cachedName.c_str(), name.c_str());
	return false;
}

bool Key::GetValue(const std::string &name, std::string &result)
{
	if (!Exists())
	{
		logger.Warn("GetValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	hive_type type;
	size_t size = 0;
	auto value = hivex_node_get_value(_hive, _node, name.c_str());
	if (value != 0 && hivex_value_type(_hive, value, &type, &size) == 0 && type == hive_t_REG_SZ)
	{
		auto contents = make_cunique(hivex_value_string(_hive, value));
		if (contents != nullptr)
		{
			result = contents.get();
			logger.Debug("GetValue %s\\%s: %s", _cachedName.c_str(), name.c_str(), contents.get());
			return true;
		}
	}

	logger.Debug("GetValue: value %s\\%s not found", _cachedName.c_str(), name.c_str());
	return false;
}

bool Key::GetValue(const std::string &name, std::vector<std::string> &result)
{
	if (!Exists())
	{
		logger.Warn("GetValue: parent key %s not found", _cachedName.c_str());
		return false;
	}

	hive_type type;
	size_t size = 0;
	auto value = hivex_node_get_value(_hive, _node, name.c_str());
	if (value != 0 && hivex_value_type(_hive, value, &type, &size) == 0 && type == hive_t_REG_MULTI_SZ)
	{
		auto contents = make_cunique(hivex_value_multiple_strings(_hive, value));
		if (contents != nullptr)
		{
			uint32_t count = 0;
			result.clear();
			for (char **ptr = contents.get(); *ptr != nullptr && **ptr != 0; ++ptr)
			{
				auto str = make_cunique(*ptr);
				result.emplace_back(str.get());
				logger.Debug("GetValue %s\\%s[%u]: %s", _cachedName.c_str(), name.c_str(), count++, str.get());
			}
			if (result.empty())
			{
				logger.Debug("GetValue %s\\%s: <empty>", _cachedName.c_str(), name.c_str());
			}
			return true;
		}
	}

	logger.Debug("GetValue: value %s\\%s not found", _cachedName.c_str(), name.c_str());
	return false;
}

bool Key::Delete()
{
	if (!Exists())
	{
		logger.Warn("GetValue: key %s not found", _cachedName.c_str());
		return false;
	}

	if (hivex_node_delete_child(_hive, _node) == 0)
	{
		logger.Debug("Delete: %s", _cachedName.c_str());
		return true;
	}

	logger.Error("Delete: %s failed", _cachedName.c_str());
	return false;
}

vector<UntypedRegistryValue> Key::GetValues()
{
	logger.Debug("GetValues: %s", _cachedName.c_str());

	vector<UntypedRegistryValue> values;

	if (!Exists())
	{
		logger.Warn("GetValues: key %s does not exist!", _cachedName.c_str());
		return values;
	}

	auto hValues = make_cunique(hivex_node_values(_hive, _node));
	if (!hValues)
	{
		logger.Error("hivex_node_values returned null! %d: %s", errno, strerror(errno));
		return values;
	}

	for (auto value = hValues.get(); value != nullptr && *value != 0; ++value)
	{
		UntypedRegistryValue val;
		val.Name = make_cunique(hivex_value_key(_hive, *value));
		val.Value = make_cunique(hivex_value_value(_hive, *value, &val.Type, &val.Length));

		values.push_back(std::move(val));
	}

	return values;
}

bool Key::SetValues(const std::vector<UntypedRegistryValue> &values)
{
	logger.Debug("GetValues: %s", _cachedName.c_str());

	if (!Exists())
	{
		logger.Warn("SetValues: key %s does not exist!", _cachedName.c_str());
		return false;
	}

	bool success = true;
	for (const auto &value : values)
	{
		hive_set_value newValue;
		newValue.key = value.Name.get();
		newValue.t = value.Type;
		newValue.len = value.Length;
		newValue.value = value.Value.get();

		if (hivex_node_set_value(_hive, _node, &newValue, 0) != 0)
		{
			logger.Error("SetValues: hivex_node_set_value error on %s! %d: %s", newValue.key, errno, strerror(errno));
			success = false;
		}
	}

	return success;
}
