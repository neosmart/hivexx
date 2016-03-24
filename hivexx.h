#pragma once

#include <hivex.h>
#include <vector>
#include <string>
#include <memory>
#include <functional>

/* hivexx C++ wrapper for libhivex
 * Everything in the registry is a key
 * A key can have a value, subkeys, or both
 * e.g. HKLM\SYSTEM is a hive
 * HKLM\SYSTEM is also a node
 * HKLM\SYSTEM is also a key (name = "", value = <anything>)
 * HKLM\SYSTEM also contains subkeys Select, CurrentControlSet, etc.
 * However, libhivex treats nodes and keys differently
*/

namespace hivexx
{
	class Node;
	class Key
	{
		friend class Node;
	private:
		Key(const Node &node, hive_value_h value, const std::string &name);
	protected:
		hive_value_h _value = 0;
		hive_node_h _node = 0;
		hive_h *_hive = nullptr;
		std::string _cachedPath;
		std::string _cachedName;
	public:
		Key() = default;
		const std::string &Name();
		bool Exists() const;
		bool GetValue(int32_t &result);
		bool GetValue(std::string &result);
		bool SetValue(int32_t value);
		bool SetValue(std::string value);
		template <typename T>
		bool ChangeIfNotEqualTo(T &&compare);
	};

	class Node
	{
		friend class Key;
	protected:
		std::string _cachedName;
		Node(hive_h *hive, hive_node_h node, const std::string &name);
		hive_node_h _node = 0;
		hive_h *_hive = nullptr;
	public:
		Node() = default;
		bool Exists() const;
		Key GetKey(const std::string &name);
		std::vector<Key> GetKeys();
		bool SetKeys(const std::vector<Key> &keys);
		Node GetNode(const std::string &path, bool create = true);
		std::vector<Node> GetNodes();
		bool DeleteKey(const std::string &name);
		bool DeleteNode(const std::string &path);
		Node CreateNode(const std::string &path);
		bool Delete();
		const std::string &Name() const;
	};

	class Hive : public Node
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
