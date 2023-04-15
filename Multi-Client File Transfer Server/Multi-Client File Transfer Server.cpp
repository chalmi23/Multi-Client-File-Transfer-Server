#include <iostream>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <vector>
#include <map>
#include <string>
#include <string.h>
#include <stdexcept>
#include <sys/stat.h>
#define SERVER_PORT "12345" // Randomowy port
#define MAX_HEADER_BUFF_LEN 256 // Maksymalna dlugosc bufora naglowkow
#define HEADER_END_CHARACTER '\n' // Znak oznaczajacy koniec naglowka
#define DOWNLOAD_REQUEST "downld" // flaga oznaczajaca ze klient chce pobrac cos z serwera
#define UPLOAD_REQUEST "upload" // flaga ze klient chce wrzucic cos na serwer
#define TRANSFER_BUFFER_LENGTH 32768 // max dlugosc bufora do transferu danych (wysylanie / odbieranie)
// Enum ktory ulatwia rozpoznawanie co serwer ma zrobic z danym klientem
// WAITING - serwer oczekuje na naglowek z zadaniem (upload / download)
// DOWNLOAD - klient zazadal pobrania jakiegos pliku z serwera, wysylane sa dane
// UPLOAD - klient zazadal wyslania jakiegos pliku na serwer, odbierane sa dane
enum Request {
	WAITING,
	DOWNLOAD,
	UPLOAD
};
// Struktura z podstawowymi info o danym kliencie
// address to IPV4 adres klienta w formacie "presentable"
// request to w jakim stanie jest klient - co chce zrobic - jak go ma serwer obsluzyc
// file to wskaznik do pliku ktory jest powiazany z aktualnym zadaniem klienta
// fileSize to rozmiar pliku ktory klient chce pobrac/zapisac
struct ClientInfo {
	std::string address;
	Request request;
	FILE* file;
	std::string fileSize;
};
void closeConnection(int socketFD, ClientInfo& clientInfo, std::vector<struct pollfd>& conns, std::map<int,
	ClientInfo>& infos);
// Funkcja agregujaca instrukcje tworzenia nowego socket'a
// zwraca deskryptor socketa albo -1 jezeli wystapil blad
int setup()
{
	// Wypelnic hints odnosnie jakiego adresu szukamy
	struct addrinfo hints = { 0 };
	struct addrinfo* res = { 0 };
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	int status;
	// NULL w adresie bo tworzymy socketa na tym komputerze
	status = getaddrinfo(nullptr, SERVER_PORT, &hints, &res);
	if (status != 0) {
		perror("Blad przy getaddrinfo()");
		return -1;
	}
	int serverSocket;
	serverSocket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (serverSocket < 0) {
		perror("Blad przy socket() servera");
		freeaddrinfo(res);
		return -1;
	}
	// Ustawiamy ponowne wykorzystanie adresu (a dokladnie portu) zeby uniknac bledow
	// ze nie da sie przypisac tego serwera do danego portu, zdaza sie najczesciej kiedy
	// kilka razy pod rzad wlaczy / wylaczy sie program serwera
	int yes = 1;
	if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
		perror("Blad przy setsockopt()");
		freeaddrinfo(res);
		close(serverSocket);
		return -1;
	}
	// Przypisz socketa do danego portu, nr portu zostal przekazany do funkcji
	// getaddrinfo() i tutaj sie kryje pod res->ai_addr
	if (bind(serverSocket, res->ai_addr, res->ai_addrlen) == -1) {
		perror("Blad przy bind()");
		freeaddrinfo(res);
		close(serverSocket);
		return -1;
	}
	freeaddrinfo(res);
	return serverSocket;
}
// Funkcja agregujaca instrukcje wymagane do akceptowania nowego polaczenia przez serwer
void acceptNewConnection(int serverSocket,
	std::vector<struct pollfd>& vect,
	std::map<int, ClientInfo>& infos)
{
	struct sockaddr_in clientAddress;
	socklen_t clientLen = sizeof(clientAddress);
	// Akceptowane jest nowe polaczenie i dane na jego temat zapisywane sa w obiekcie clientAddress
	// struktury sockaddr_in
	int newSocket;
	newSocket = accept(serverSocket, (struct sockaddr*)&clientAddress, &clientLen);
	if (newSocket == -1) {
		perror("Blad przy tworzeniu nowego socketa dla nowego polaczenia");
		return;
	}
	// Te nowe polaczenie dodawane jest do wektora struct pollfd, ktory reprezentuje wszystkie aktywne
	// polaczenia, wektor jest przekazany przez referencje a wiec zostanie poprawnie dopisana ta nowa wartosc
	struct pollfd newPollFD;
	newPollFD.fd = newSocket;
	newPollFD.events = POLLIN;
	vect.push_back(newPollFD);
	// Ustalany jest "presentable" adres IPV4 klienta na podstawie info na jego temat ze struktury
	// sockaddr_in
	ClientInfo ci;
	char addr[INET_ADDRSTRLEN];
	if (inet_ntop(clientAddress.sin_family, &(clientAddress.sin_addr), addr, clientLen) != nullptr)ci.address =
		std::string(addr);
	else ci.address = std::string("unknown_addr");
	// Ustawiany jest request type na WAITING bo teraz serwer musi czekac na naglowek z zadaniem od klienta
	ci.request = WAITING;
	ci.file = nullptr;
	// Ten obiekt zostanie poprawnie dodany do mapy z main()'a bo mapa jest przekazana przez referencje
	infos.insert({ newSocket, ci });
}
// Funkcja ktora szuka w buforze danych znaku konca naglowka
// zwraca true jezeli znajdzie
// zwraca false jezeli nie znajdzie
bool lookForEndOfHeader(std::vector<char>& buff)
{
	for (int i = 0; i < buff.size(); i++) {
		if (buff[i] == HEADER_END_CHARACTER) {
			return true;
		}
	}
	return false;
}
// Funkcja ktora jest odpowiedzialna za otrzymywanie danych naglowkowych klientow, a nastepnie rozpoznawanie
// na podstawie przeslanego naglowka co klient chce zrobic (zadanie klienta)
void newConnectionAction(struct pollfd& pfd,
	ClientInfo& clientInfo,
	std::map<int, std::vector<char>>& headerBuffers,
	std::vector<struct pollfd>& conns,
	std::map<int, ClientInfo>& infos)
{
	// Jezeli ten klient nie ma jeszcze bufora to dodac bufor dla tego klienta
	if (headerBuffers.find(pfd.fd) == headerBuffers.end()) {
		headerBuffers.insert({ pfd.fd, std::vector<char>() });
	}
	char buff[MAX_HEADER_BUFF_LEN] = { 0 };
	ssize_t recSize = 0;
	recSize = recv(pfd.fd, buff, MAX_HEADER_BUFF_LEN - headerBuffers.at(pfd.fd).size(), 0);
	if (recSize <= 0) {
		// Blad lub klient zakonczyl polaczenie
		if (recSize < 0) {
			perror("Blad przy odbieraniu naglowka");
		}
		// Usun bufor naglowka tego klienta
		headerBuffers.at(pfd.fd).clear();
		headerBuffers.erase(pfd.fd);
		// Zamknij to polaczenie / socketa
		close(pfd.fd);
		// Usun informacje tego klienta
		infos.erase(pfd.fd);
		// Usun tego klienta z listy aktywnych polaczen
		for (auto it = conns.begin(); it != conns.end(); it++) {
			if (it->fd == pfd.fd) {
				conns.erase(it);
				break;
			}
		}
		return;
	}
	int i = 0; // ten int jest porownywany do "signed ssize_t" oraz "unsigned size_t"
	// ale jako ze jego wartosc nigdy nie powinna byc zbyt duza to nigdy nie
	// powinno byc problemow z porownywaniem "signed and unsigned values"
	// Dodaj zawartosc bufora buff do bufora dedykowanego dla tego klienta
	while (i <= recSize) {
		headerBuffers.at(pfd.fd).push_back(buff[i]);
		// Jezeli klient podal zly naglowke -> zbyt duzy naglowek
		if (headerBuffers.at(pfd.fd).size() > MAX_HEADER_BUFF_LEN) {
			// Posprzataj po tym polaczeniu
			std::cerr << "Otrzymany naglowek przekroczyl dozwolone rozmiary" << std::endl;
			headerBuffers.at(pfd.fd).clear();
			headerBuffers.erase(pfd.fd);
			close(pfd.fd);
			infos.erase(pfd.fd);
			for (auto it = conns.begin(); it != conns.end(); it++) {
				if (it->fd == pfd.fd) {
					conns.erase(it);
					break;
				}
			}
			return;
		}
		i++;
	}
	// Sproboj znalezc znak konczacy naglowke w buforze
	if (lookForEndOfHeader(headerBuffers.at(pfd.fd)))
	{
		// Tymczasowy string do latwiejszej pracy, bufowany na podstawie danych z vector<char>
		std::string tmpStr(headerBuffers.at(pfd.fd).begin(), headerBuffers.at(pfd.fd).end());
		// PRZYGOTOWYWANIE DO WYSYŁANIA PLIKU DO KLIENTA
		if (tmpStr.find(DOWNLOAD_REQUEST) != std::string::npos)
		{
			// Ustalanie events tego klienta na POLLOUT bo dane beda wysylane do klienta
			pfd.events = POLLOUT;
			// Ustawianie info dot. obslugi tego klienta na DOWNLOAD
			clientInfo.request = DOWNLOAD;
			// Budowanie string'a zawierajacego sciezke / nazwe pliku ktory klient chce pobrac
			// Trzeba "przeskoczyc" flage "downld" / "upload" w buforze zeby dostac sie do nazwy pliku
			std::string filename;
			// nazwa pliku zaczyna się od znaku ":" do znaku "\n"
			i = sizeof(DOWNLOAD_REQUEST);
			while (headerBuffers.at(pfd.fd)[i] != HEADER_END_CHARACTER) {
				filename += (headerBuffers.at(pfd.fd))[i];
				i++;
			}
			// wyswietl nazwe pliku
			std::cout << "Nazwa pliku: " << filename << std::endl;
			// Sproboj otworzyc ten plik
			clientInfo.file = fopen(filename.c_str(), "r");
			// Jezeli nie udalo sie otworzyc tego pliku to najprawdopodobniej nie ma takiego pliku, lub
			// jakis inny niespodziewany blad sie wydarzyl, tak czy siak - polaczenie jest zrywane
			if (clientInfo.file == nullptr) {
				std::cout << "blad przy otwieraniu pliku do odczytu do wyslania do klienta" << std::endl;
				headerBuffers.at(pfd.fd).clear();
				headerBuffers.erase(pfd.fd);
				close(pfd.fd);
				infos.erase(pfd.fd);
				for (auto it = conns.begin(); it != conns.end(); it++) {
					if (it->fd == pfd.fd) {
						conns.erase(it);
						break;
					}
				}
				return;
			}
			// sprawdź wielkość pliku clientInfo.file funkcją stat i przypisz ją do pola fileSize w strukturze
			clientInfo
				struct stat st;
			if (stat(filename.c_str(), &st) == 0)
			{
				std::cout << "Wielkosc pliku: " << st.st_size << std::endl;
				clientInfo.fileSize = std::to_string(st.st_size);
				// wysłanie do klienta nagłówka z wielkością pliku
				std::string header = "SIZE:" + clientInfo.fileSize + "\n";
				send(pfd.fd, header.c_str(), header.size(), 0);
				std::cout << "przesłany nagłówek: " << header << std::endl;
			}
			else
			{
				// wyswietl blad przy stat oraz zamknij polaczenie
				std::cout << "blad przy stat" << std::endl;
				headerBuffers.at(pfd.fd).clear();
				headerBuffers.erase(pfd.fd);
				close(pfd.fd);
				infos.erase(pfd.fd);
				for (auto it = conns.begin(); it != conns.end(); it++) {
					if (it->fd == pfd.fd) {
						conns.erase(it);
						break;
					}
				}
				return;
			}
		}
		else if (tmpStr.find(UPLOAD_REQUEST) != std::string::npos)
		{
			std::string fileSize;
			// zapisanie wielkości pliku do zmiennej fileSize
			i = sizeof(UPLOAD_REQUEST);
			while (headerBuffers.at(pfd.fd)[i] != HEADER_END_CHARACTER) {
				fileSize += (headerBuffers.at(pfd.fd))[i];
				i++;
			}
			// wyswietl wielkosc pliku
			std::cout << "Wielkosc pliku: " << fileSize << std::endl;
			clientInfo.fileSize = fileSize;
			std::cout << clientInfo.fileSize << std::endl;
			std::cout << "Wysyłanie pliku na serwer" << std::endl;
			// ^ Sprawdzanie czy klient wyslal flage "chce wrzucic cos na serwer"
			// events klienta powinno dalej byc POLLIN ale jest jeszcze raz przypisywane dla pewnosci
			// przypisywane jest POLLIN bo klient bedzie przesylal dane do serwera
			pfd.events = POLLIN;
			// Ustawiane jest info klienta na UPLOAD -> klient wrzuca dane na serwer
			clientInfo.request = UPLOAD;
			// Tworzona jest nazwa pliku jaki klient wrzuca na serwer, nazwa pliku ma format
			// <adres ipv4 klienta>-<numer pliku przesylanego na serwer>
			static int counter = 0;
			std::string filename(clientInfo.address);
			filename += "-";
			filename += std::to_string(counter);
			counter++;
			// Tworzony jest ten plik
			clientInfo.file = fopen(filename.c_str(), "w");
			if (clientInfo.file == nullptr) {
				std::cout << "blad przy otwieraniu pliku do odczytu do wyslania do klienta" << std::endl;
				headerBuffers.at(pfd.fd).clear();
				headerBuffers.erase(pfd.fd);
				close(pfd.fd);
				infos.erase(pfd.fd);
				for (auto it = conns.begin(); it != conns.end(); it++) {
					if (it->fd == pfd.fd) {
						conns.erase(it);
						break;
					}
				}
				return;
			}
			// Znajdz indeks znaku konczacego naglowek
			size_t j = 0;
			while (tmpStr[j] != HEADER_END_CHARACTER) {
				j++;
			}
			// Sprawdzic czy klient przeslal tez jakies dane poza naglowkiem
			if (j < (tmpStr.length() - 2)) {
				fwrite(tmpStr.substr(j).c_str(), 1, tmpStr.length() - j - 1, clientInfo.file);
			}
}
		else
		{
			// Zle dane
			std::cout << "Zle dane w naglowku!" << std::endl; //cipka do zmiany
			close(pfd.fd);
			infos.erase(pfd.fd);
			for (auto it = conns.begin(); it != conns.end(); it++) {
				if (it->fd == pfd.fd) {
					conns.erase(it);
					break;
				}
			}
}
// Jezeli doszlismy tutaj to nalezy usunac bufor naglowka tego klienta bo klient przeslal poprawny
naglowek
headerBuffers.at(pfd.fd).clear();
headerBuffers.erase(pfd.fd);
return;
}
else
{
	// Nie caly naglowkek przeslano
	return;
}
}
// Funkcja odpowiedzialna za wyslanie do klienta kawalku pliku o dlugosci TRANSFER_BUFFER_LENGTH
void downloadAction(int socketFD,
	ClientInfo& clientInfo,
	std::vector<struct pollfd>& conns,
	std::map<int, ClientInfo>& infos)
{
	size_t readSize = 0;
	size_t sentSize = 0;
	char buff[TRANSFER_BUFFER_LENGTH] = { 0 };
	int wyslanyBuffor = 0;
	// Przekowertuj stringa clientInfo.fileSize na size_t
	size_t fileSize = std::stoull(clientInfo.fileSize);
	if (fileSize > 0)
	{
		// przesłunięcie wskaźnika
		fseek(clientInfo.file, clientInfo.odKiedyOdczytaj, SEEK_SET);
		// Pętla wysyłająca dane ramkami o rozmiarze TRANSFER_BUFFER_LENGTH
		readSize = fread(buff, 1, TRANSFER_BUFFER_LENGTH, clientInfo.file);
		if (readSize < TRANSFER_BUFFER_LENGTH) {
			if (feof(clientInfo.file) != 0) {
				// Jeżeli koniec pliku to wysyłamy tylko to co jest jeszcze do wysłania
				readSize = fileSize - clientInfo.odKiedyOdczytaj;
			}
			else {
				std::cerr << "Blad przy czytaniu pliku!" << std::endl;
				closeConnection(socketFD, clientInfo, conns, infos);
				return;
			}
		}
		if (wyslanyBuffor = send(socketFD, buff, readSize, 0) < 0) {
			std::cerr << "Blad przy wysylaniu danych do klienta!" << std::endl;
			closeConnection(socketFD, clientInfo, conns, infos);
			return;
		}
		fileSize -= wyslanyBuffor;
		clientInfo.odKiedyOdczytaj += wyslanyBuffor;
	}
	else
	{
		// Zamknij plik bo caly byl wyslany
		fclose(clientInfo.file);
		clientInfo.file = nullptr;
		// Zmien sposob obslugi klienta na WAITING bo serwer bedzie czekac na nowy naglowek
		clientInfo.request = WAITING;
		// Jako ze bedzie sie przechodzic z wysylania do obecnego klienta danych do
		// oczekiwania (od niego) na naglowek nowy, to trzeba zmienic z POLLOUT na POLLIN
		for (auto& pfd : conns) {
			if (pfd.fd == socketFD) {
				pfd.events = POLLIN;
			}
		}
	}
}
// Funkcja odpowiadajaca za odebranie od klienta czesci danych o max dlugosci TRANSFER_BUFFER_LENGTH
void uploadAction(int socketFD,
	ClientInfo& clientInfo,
	std::vector<struct pollfd>& conns,
	std::map<int, ClientInfo>& infos)
{
	size_t fileSize = std::stoull(clientInfo.fileSize);
	ssize_t receivedSize = 0;
	char buff[TRANSFER_BUFFER_LENGTH] = { 0 };
	while (fileSize > 0) {
		receivedSize = recv(socketFD, buff, (TRANSFER_BUFFER_LENGTH < fileSize ? TRANSFER_BUFFER_LENGTH : fileSize),
			0);
		if (receivedSize <= 0) {
			if (receivedSize == 0) {
				// Klient zamknal polaczenie
				std::cout << "Klient zakonczyl polaczenie" << std::endl;
			}
			else {
				// Wystapil blad
				std::cerr << "Blad przy otrzymywaniu danych od klienta" << std::endl;
			}
			// Tak czy siak nalezy zamknac polaczenie i posprzatac
			closeConnection(socketFD, clientInfo, conns, infos);
			return;
		}
		fileSize -= receivedSize;
		fwrite(buff, sizeof(char), receivedSize, clientInfo.file);
	}
	fclose(clientInfo.file);
	clientInfo.file = nullptr;
	clientInfo.request = WAITING;
	std::cout << "Plik zostal zapisany" << std::endl;
	for (auto& pfd : conns) {
		if (pfd.fd == socketFD) {
			pfd.events = POLLIN;
		}
	}
}
int main()
{
	// Stworz nowy socket serwera
	int serverSocket;
	serverSocket = setup();
	if (serverSocket == -1) {
		std::cout << "Nie utworzono poprawnie seocket'u servera!" << std::endl;
		return -1;
	}
	if (listen(serverSocket, 10) == -1) { // max bakclog == 10
		perror("Blad przy listen()");
		close(serverSocket);
		return -1;
	}
	// Dane potrzebne do pracy serwera
	std::vector<struct pollfd> connections; // Wektor odpowiadajacy za liste aktywnych polaczen serwera
	struct pollfd servPFD; // Obiekt pollfd z danymi serwera
	servPFD.fd = serverSocket;
	servPFD.events = POLLIN;
	connections.push_back(servPFD); // Dodaj ten obiekt pollfd do listy polaczen
	int howMany = 0; // Liczba ile polaczen jest gotowych do obslugi
	std::map<int, ClientInfo> informations; // Mapa z danymi ClientInfo na temat kazdego z aktywnych polaczen
	struct pollfd* vectData; // Poniewaz nie mozna do poll() przekazac wektora nalezy skorzystac z faktu ze
	// dane w wektorze sa poukladane "jedne kolo drugiego" jak w tablicy z C
	std::map<int, std::vector<char>> headerBuffers; // Mapa z buforami naglowkow kazdego z klientow
	// Glowna logika serwera
	while (true) {
		// pobierz dane z wektora do tablicy
		vectData = connections.data();
		// sprawdz ile polaczen jest gotowych
		howMany = poll(vectData, connections.size(), -1);
		// sprawdzanie bledow
		if (howMany == -1) {
			perror("Blad przy poll()!");
			// Zamknij wszystkie polaczenia
			for (auto& pfd : connections) { close(pfd.fd); }
			// Zamknij wszystkie pliki
			for (auto& info : informations) {
				if (info.second.file != nullptr) {
					fclose(info.second.file);
				}
			}
			return -1;
		}
		// Obsluga kazdego z polaczen
		for (int i = 0; i < connections.size(); i++) {
			// Sprawdz czy aktualne polaczenie jest serwerem
			if (connections[i].fd == serverSocket) {
				// jezeli serwer jest gotowy
				if (connections[i].revents & POLLIN) {
					// to go obsluz
					acceptNewConnection(serverSocket, connections, informations);
				}
			}
			else {
				// Inaczej jest obsluga klienta, na podstawie flagi request
				switch (informations.at(connections[i].fd).request) {
				case WAITING:
					if (connections[i].revents & POLLIN) {
						newConnectionAction(connections[i],
							informations.at(connections[i].fd),
							headerBuffers,
							connections,
							informations);
					}
					break;
				case DOWNLOAD:
					if (connections[i].revents & POLLOUT) {
						downloadAction(connections[i].fd,
							informations.at(connections[i].fd),
							connections,
							informations);
					}
					break;
				case UPLOAD:
					if (connections[i].revents & POLLIN) {
						uploadAction(connections[i].fd,
							informations.at(connections[i].fd),
							connections,
							informations);
					}
					break;
				}
			}
		}
	}
	// de facto tutaj serwer nigdy nie dojdzie, ale jest clean up w razie czego
	for (auto& info : informations) {
		fclose(info.second.file);
	}
	for (auto& pfd : connections) {
		close(pfd.fd);
	}
	return 0;
}
void closeConnection(int socketFD, ClientInfo& clientInfo, std::vector<struct pollfd>& conns, std::map<int,
	ClientInfo>& infos)
{
	fclose(clientInfo.file);
	close(socketFD);
	infos.erase(socketFD);
	for (auto it = conns.begin(); it != conns.end(); it++)
	{
		if (it->fd == socketFD)
		{
			conns.erase(it);
		}
	}
}
