/*
 * Copyright (C) 2018  Alexander Kernozhitsky <sh200105@mail.ru>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UTILS_H
#define UTILS_H

#include <string>

namespace UnixRunner {

bool fileIsGood(const char *fileName);
bool fileIsGood(const std::string &fileName);

bool directoryIsGood(const char *fileName);
bool directoryIsGood(const std::string &fileName);

bool fileIsReadable(const char *fileName);
bool fileIsReadable(const std::string &fileName);

bool fileIsWritable(const char *fileName);
bool fileIsWritable(const std::string &fileName);

bool fileIsExecutable(const char *fileName);
bool fileIsExecutable(const std::string &fileName);

bool updateLimit(int resource, int64_t value);

std::string demangle(const char *typeName);

std::string getFullExceptionMessage(const std::exception &exc);

}  // namespace UnixRunner

#endif  // UTILS_H
