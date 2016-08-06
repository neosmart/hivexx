#pragma once

#include <hivex.h>
#include <vector>
#include <string>
#include <memory>
#include <functional>

/* hivexx C++ wrapper for libhivex
 * Everything in the registry is a key, value, or both
 * Key: aka "registry folder", or what you see in the regedit sidebar
 * Value: <name, value> pair of type DWORD/QWORD/REG_SZ/etc
 * All keys are also values, which is the "unnamed value" in regedit
*/

namespace hivexx
{
	template<typename T>
	using cunique = std::unique_ptr<T, std::function<void(T*)>>;

	struct UntypedRegistryValue
	{
		cunique<char> Name;
		hive_type Type;
		cunique<char> Value;
		size_t Length;
	};

	template<typename T>
	struct RegistryValue:
		UntypedRegistryValue
	{
		T& Value;
	};

	class Key
	{
		friend class Value;
	protected:
		std::string _cachedName;
		Key(hive_h *hive, hive_node_h node, const std::string &name);
		hive_node_h _node = 0;
		hive_h *_hive = nullptr;
	public:
		Key() = default;
		bool Exists() const;
		std::vector<UntypedRegistryValue> GetValues();
		bool SetValues(const std::vector<UntypedRegistryValue> &values);
		Key GetSubkey(std::string path, bool create = false);
		std::vector<Key> GetSubkeys();
		bool Delete();
		bool DeleteValue(const std::string &name);
		bool DeleteSubkey(const std::string &path);
		Key CreateSubkey(const std::string &path);
		const std::string &Name() const;

		bool HasValue(const std::string &name) const;
		bool GetValue(const std::string &name, int32_t &result);
		bool GetValue(const std::string &name, std::string &result);
		bool GetValue(const std::string &name, std::vector<std::string> &result);
		bool SetValue(const std::string &name, int32_t value);
		bool SetValue(const std::string &name, const std::string &value);
		bool SetValue(const std::string &name, const std::vector<std::string> &values);
		template <typename T>
		bool ChangeIfNotEqualTo(const std::string &name, T &&compare, bool orNotFound = false);
	};

	class Hive : public Key
	{
		struct HiveWrapper
		{
		private:
			hive_h *_hive;
		public:
			HiveWrapper(hive_h *hive);
			~HiveWrapper();
		};
	private:
		std::string _path;
		std::shared_ptr<HiveWrapper> _hiveWrapper;

	public:
		Hive() = default;
		bool Load(const std::string &path);
		const std::string &Path() const;
		~Hive();
		bool Save();
	};
}
